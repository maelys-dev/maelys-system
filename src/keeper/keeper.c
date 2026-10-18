/*
 * The keeper: one process per owned program, outside its group. It is
 * started by the host with a life pipe on 3, a control socket on 4 and the
 * program's descriptors from 5, reads the program to start from the GO
 * body, starts it as the leader of a new process group X, and then does
 * three things until it leaves: it signals the group when asked, it kills
 * the group the instant the owner is lost, and it observes the leader's
 * end without reaping it, so that the number X stays taken until its last
 * signal is sent.
 *
 * Rules this file keeps:
 * - Nothing blocks but poll: every read and write is non-blocking, every
 *   reaping is WNOHANG, every wait has a deadline.
 * - No real signal to -X once the leader is reaped. A signal 0 acts on
 *   nobody and is the only probe afterwards.
 * - Every signal but KILL and STOP is blocked and read in the loop: a
 *   signalfd on Linux, EVFILT_SIGNAL on macOS. SIGCHLD stays SIG_DFL so
 *   that the leader stays a zombie until reaped here.
 * - Nothing is allocated after the program's exec; what the keeper needs
 *   is acquired before. A failure after it never reaps the leader early:
 *   with the reservation held, the group is killed and reclaimed; without
 *   it, the observation goes on and the failure is reported.
 * - The exit code is the keeper's own terminal state, never the
 *   program's: 0 ended, 1 uncertain, 2 not started, 3 failed closed.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "maelys/sys/clock.h"
#include "maelys/sys/owned.h"
#include "maelys/sys/process.h"
#include "owned_protocol.h"
#include "process_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/signalfd.h>
#else
#include <sys/event.h>
#endif

#define QUEUE_SIZE 32u
#define DONE_ACK_WAIT_MS 100u

/* A trace of what the keeper does, appended to the file named at compile
 * time by MAELYS_SYS_KEEPER_TRACE; nothing in an ordinary build. */
#if defined(MAELYS_SYS_KEEPER_TRACE)
#include <stdarg.h>
#include <stdio.h>
static void trace(const char *format, ...) {
    FILE *file = fopen(MAELYS_SYS_KEEPER_TRACE, "a");
    if (!file) return;
    va_list arguments;
    va_start(arguments, format);
    fprintf(file, "[%ld] ", (long)getpid());
    vfprintf(file, format, arguments);
    fputc('\n', file);
    va_end(arguments);
    fclose(file);
}
#else
#define trace(...) ((void)0)
#endif

typedef struct results {
    uint32_t seq[OWNED_RESULTS_KEPT];
    int32_t value[OWNED_RESULTS_KEPT];
    size_t next;
    size_t count;
} results_t;

typedef struct keeper {
    int life, ctl, sigfd, owner_fd, report_fd;
    int ctl_closed;
    /* the program */
    char *body;
    size_t body_length, body_got;
    int awaiting_body;
    char **argv, **envp;
    maelys_sys_process_fd_t fds[MAELYS_SYS_OWNED_MAX_FDS];
    maelys_sys_process_options_t program;
    unsigned flags;
    uint64_t heartbeat_ms, observe_ms, linger_ms;
    /* the group */
    pid_t x;
    int x_created;      /* a child exists */
    int exec_confirmed; /* the program runs: the barrier is behind */
    int started_written;
    int leader_ended, leader_reaped;
    int leader_exited, leader_signaled, leader_code;
    int signal_refused, members_present;
    int failure;
    int armed, owner_lost, aborted, stopping, killed;
    maelys_sys_owned_phase_t phase;
    uint32_t stop_seq;
    int have_stop_seq;
    /* deadlines, absolute, 0 when unarmed */
    uint64_t start_deadline, grace_deadline, observe_deadline, linger_deadline;
    uint64_t next_beat, next_probe, done_deadline;
    uint64_t probe_interval;
    int done_queued, done_acked;
    /* channel */
    owned_record_t queue[QUEUE_SIZE];
    size_t queue_head, queue_count, queue_sent;
    uint8_t rbuf[OWNED_RECORD_SIZE];
    size_t rlen;
    results_t results;
    uint32_t highest_seq;
    int have_seq;
} keeper_t;

static uint64_t now_ms(void) {
    uint64_t now = 0;
    (void)maelys_sys_monotonic_ms(&now);
    return now;
}

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* --- the channel ------------------------------------------------------ */

static owned_record_t make_record(uint16_t type) {
    owned_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = OWNED_MAGIC;
    record.protocol = OWNED_PROTOCOL;
    record.type = type;
    return record;
}

static unsigned phase_flags(const keeper_t *k) {
    unsigned flags = 0;
    if (k->leader_ended && !k->leader_reaped && k->killed) flags |= OWNED_F_LEADER_UNREAPED;
    if (k->signal_refused) flags |= OWNED_F_SIGNAL_REFUSED;
    if (k->members_present) flags |= OWNED_F_MEMBERS_PRESENT;
    return flags;
}

/* Queues a record; BEAT and PHASE replace a pending one of their kind. */
static void enqueue(keeper_t *k, owned_record_t record) {
    if (k->ctl_closed) return;
    if (record.type == OWNED_K_BEAT || record.type == OWNED_K_PHASE) {
        for (size_t i = 0; i < k->queue_count; ++i) {
            size_t at = (k->queue_head + i) % QUEUE_SIZE;
            if (k->queue[at].type == record.type && !(i == 0 && k->queue_sent)) {
                k->queue[at] = record;
                return;
            }
        }
    }
    if (k->queue_count == QUEUE_SIZE) {
        /* Full: a beat may be dropped, nothing else. */
        if (record.type == OWNED_K_BEAT) return;
        /* Drop the oldest beat or phase to make room. */
        for (size_t i = 1; i < k->queue_count; ++i) {
            size_t at = (k->queue_head + i) % QUEUE_SIZE;
            if (k->queue[at].type == OWNED_K_BEAT || k->queue[at].type == OWNED_K_PHASE) {
                for (size_t j = i; j + 1 < k->queue_count; ++j) {
                    k->queue[(k->queue_head + j) % QUEUE_SIZE] = k->queue[(k->queue_head + j + 1) % QUEUE_SIZE];
                }
                --k->queue_count;
                break;
            }
        }
        if (k->queue_count == QUEUE_SIZE) return;
    }
    k->queue[(k->queue_head + k->queue_count) % QUEUE_SIZE] = record;
    ++k->queue_count;
}

static void send_phase(keeper_t *k) {
    owned_record_t record = make_record(OWNED_K_PHASE);
    record.a = (uint32_t)k->phase;
    record.b = phase_flags(k);
    record.value = k->failure;
    record.pid = (int32_t)k->x;
    enqueue(k, record);
}

static void send_ack(keeper_t *k, uint32_t seq, int value) {
    owned_record_t record = make_record(OWNED_K_ACK);
    record.seq = seq;
    record.value = value;
    enqueue(k, record);
}

/* Pushes what waits, as far as the socket admits; -1 when the channel is
 * gone. STARTED counts as written only once its last byte has gone. */
static int flush(keeper_t *k) {
    while (k->queue_count && !k->ctl_closed) {
        owned_record_t *head = &k->queue[k->queue_head];
        if (head->type == OWNED_K_STARTED && k->aborted && k->queue_sent == 0) {
            /* Abandoned before a byte of it went: not sent at all. */
            k->queue_head = (k->queue_head + 1) % QUEUE_SIZE;
            --k->queue_count;
            continue;
        }
        const uint8_t *bytes = (const uint8_t *)head;
        ssize_t sent = send(k->ctl, bytes + k->queue_sent, OWNED_RECORD_SIZE - k->queue_sent,
#if defined(MSG_NOSIGNAL)
            MSG_NOSIGNAL
#else
            0
#endif
        );
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            trace("control lost on send: errno %d", errno);
            k->ctl_closed = 1;
            return -1;
        }
        trace("sent type %u, %zd bytes", (unsigned)head->type, sent);
        k->queue_sent += (size_t)sent;
        if (k->queue_sent == OWNED_RECORD_SIZE) {
            if (head->type == OWNED_K_STARTED && !k->aborted) {
                /* The last byte gone: the start is validated and its
                 * deadline is over. Not before, and not once the start
                 * was abandoned: a STARTED begun before the abandon is
                 * finished for the framing, and validates nothing. */
                k->started_written = 1;
                k->start_deadline = 0;
            }
            if (head->type == OWNED_K_DONE) k->done_queued = 2;
            k->queue_head = (k->queue_head + 1) % QUEUE_SIZE;
            --k->queue_count;
            k->queue_sent = 0;
        }
    }
    return 0;
}

/* --- the group --------------------------------------------------------- */

static void note_kill(keeper_t *k, int result) {
    if (result == 0) k->signal_refused = 0;
    else if (errno == EPERM) k->signal_refused = 1;
}

/* SIGKILL to what exists, then the reclaiming. Before the exec is
 * confirmed, on Linux, the child may still be in the keeper's group: it
 * is killed by its number too, which it reserves as an unreaped child. */
static void kill_group(keeper_t *k, uint64_t now) {
    if (!k->x_created || k->leader_reaped) return;
    if (!k->exec_confirmed) (void)kill(k->x, SIGKILL);
    int result = kill(-k->x, SIGKILL);
    note_kill(k, result);
    k->killed = 1;
    k->phase = MAELYS_SYS_OWNED_RECLAIMING;
    k->grace_deadline = 0;
    k->observe_deadline = now + k->observe_ms;
    k->probe_interval = 1;
    k->next_probe = now;
    send_phase(k);
}

static void finish(keeper_t *k, maelys_sys_owned_phase_t final, uint64_t now) {
    k->phase = final;
    owned_record_t record = make_record(OWNED_K_DONE);
    record.a = (uint32_t)final;
    record.b = phase_flags(k);
    record.value = k->failure;
    record.pid = (int32_t)k->x;
    enqueue(k, record);
    k->done_queued = 1;
    k->done_deadline = now + DONE_ACK_WAIT_MS;
}

/* The owner is lost, or the keeper was told to stop before any start: the
 * group dies now, without grace. */
static void lose(keeper_t *k, int cause, uint64_t now) {
    trace("owner lost: cause %d, x %ld created %d", cause, (long)k->x, k->x_created);
    if (k->owner_lost) return;
    k->owner_lost = 1;
    if (k->x_created && !k->leader_reaped) {
        kill_group(k, now);
        return;
    }
    if (!k->x_created) {
        owned_record_t record = make_record(OWNED_K_LOST);
        record.value = cause;
        enqueue(k, record);
        k->aborted = 1;
        finish(k, MAELYS_SYS_OWNED_START_FAILED, now);
    }
}

/* An internal failure: closed when the reservation is held, reported
 * either way (decision 8). */
static void fail(keeper_t *k, int error, uint64_t now) {
    if (!k->failure) k->failure = error ? error : EIO;
    if (k->x_created && !k->leader_reaped && !k->killed) kill_group(k, now);
    else send_phase(k);
}

/* The leader's status, observed without reaping. */
static void look_at_leader(keeper_t *k, uint64_t now) {
    if (!k->x_created || !k->exec_confirmed || k->leader_ended) return;
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int got = waitid(P_PID, (id_t)k->x, &info, WEXITED | WNOWAIT | WNOHANG);
    if (got != 0 || info.si_pid != k->x) return;
    k->leader_ended = 1;
    if (info.si_code == CLD_EXITED) {
        k->leader_exited = 1;
        k->leader_code = info.si_status;
    } else {
        k->leader_signaled = 1;
        k->leader_code = info.si_status;
    }
    owned_record_t record = make_record(OWNED_K_EXITED);
    record.a = (uint32_t)k->leader_exited;
    record.b = (uint32_t)k->leader_code;
    record.value = k->leader_signaled;
    record.pid = (int32_t)k->x;
    enqueue(k, record);
    if (k->killed) return;
    if ((k->flags & MAELYS_SYS_OWNED_END_WITH_LEADER) || k->stopping) {
        kill_group(k, now);
    } else {
        k->phase = MAELYS_SYS_OWNED_LEADER_ENDED;
        send_phase(k);
    }
}

/* After the last SIGKILL: the leader reaped once, WNOHANG, then the group
 * observed with signal 0 until it is empty or the deadline passes. */
static void reclaim(keeper_t *k, uint64_t now) {
    if (!k->killed || k->done_queued) return;
    if (!k->leader_reaped) {
        /* One attempt per turn in RECLAIMING; in UNCERTAIN, one every
         * hundred milliseconds, which is what next_probe then paces. */
        if (k->phase == MAELYS_SYS_OWNED_UNCERTAIN && now < k->next_probe) return;
        int raw = 0;
        pid_t got = waitpid(k->x, &raw, WNOHANG);
        if (got == k->x || (got < 0 && errno == ECHILD)) {
            k->leader_reaped = 1;
            if (got == k->x && !k->leader_ended) {
                k->leader_ended = 1;
                maelys_sys_process_status_t status;
                maelys_sys__process_status_fill(&status, raw);
                k->leader_exited = status.exited;
                k->leader_signaled = status.signaled;
                k->leader_code = status.exited ? status.exit_code : status.term_signal;
                owned_record_t record = make_record(OWNED_K_EXITED);
                record.a = (uint32_t)k->leader_exited;
                record.b = (uint32_t)k->leader_code;
                record.value = k->leader_signaled;
                record.pid = (int32_t)k->x;
                enqueue(k, record);
            }
            k->next_probe = now;
            if (k->phase == MAELYS_SYS_OWNED_RECLAIMING) {
                k->phase = MAELYS_SYS_OWNED_OBSERVING;
                send_phase(k);
            }
        } else if (got == 0 && k->phase == MAELYS_SYS_OWNED_RECLAIMING && now >= k->observe_deadline) {
            k->phase = MAELYS_SYS_OWNED_UNCERTAIN;
            k->linger_deadline = now + k->linger_ms;
            k->next_probe = now + 100u;
            send_phase(k);
        } else if (got == 0 && k->phase == MAELYS_SYS_OWNED_UNCERTAIN) {
            k->next_probe = now + 100u;
        }
    }
    if (k->leader_reaped && now >= k->next_probe && k->phase != MAELYS_SYS_OWNED_RECLAIMING) {
        errno = 0;
        int probe = kill(-k->x, 0);
        if (probe != 0 && errno == ESRCH) {
            k->members_present = 0;
            finish(k, MAELYS_SYS_OWNED_ENDED, now);
            return;
        }
        /* 0: a member is there. EPERM: the group exists and nothing in it
         * may be signaled: on macOS, measured, that is also what a group
         * of zombies alone answers. Undiagnosed either way. */
        k->members_present = probe == 0;
        if (probe != 0 && errno == EPERM) k->signal_refused = 1;
        if (k->probe_interval < 100) k->probe_interval *= 2;
        k->next_probe = now + k->probe_interval;
        if (k->phase == MAELYS_SYS_OWNED_OBSERVING && now >= k->observe_deadline) {
            k->phase = MAELYS_SYS_OWNED_UNCERTAIN;
            k->linger_deadline = now + k->linger_ms;
            send_phase(k);
        }
    }
    if (k->phase == MAELYS_SYS_OWNED_UNCERTAIN && now >= k->linger_deadline) {
        finish(k, MAELYS_SYS_OWNED_UNCERTAIN_FINAL, now);
    }
}

/* --- the start --------------------------------------------------------- */

static int parse_body(keeper_t *k) {
    if (k->body_length < sizeof(owned_go_head_t)) return EINVAL;
    owned_go_head_t head;
    memcpy(&head, k->body, sizeof(head));
    if (head.argc > 65536u || head.envc > 65536u || head.fdc > MAELYS_SYS_OWNED_MAX_FDS) return EINVAL;
    size_t strings_at = sizeof(head);
    size_t targets_at = strings_at + head.strings_length;
    if (head.strings_length > k->body_length - strings_at) return EINVAL;
    if ((k->body_length - targets_at) != (size_t)head.fdc * sizeof(int32_t)) return EINVAL;
    const char *strings = k->body + strings_at;
    const char *end = strings + head.strings_length;
    /* path, argv, envp, cwd: exactly argc + envc + 2 strings, each ended */
    size_t wanted = (size_t)head.argc + head.envc + 2u;
    size_t count = 0;
    for (const char *p = strings; p < end; ++p) if (*p == '\0') ++count;
    if (count != wanted || end[-1] != '\0') return EINVAL;
    k->argv = calloc((size_t)head.argc + 1u, sizeof(char *));
    k->envp = calloc((size_t)head.envc + 1u, sizeof(char *));
    if (!k->argv || !k->envp) return ENOMEM;
    const char *cursor = strings;
    k->program.path = cursor;
    cursor += strlen(cursor) + 1u;
    for (uint32_t i = 0; i < head.argc; ++i) {
        k->argv[i] = (char *)cursor;
        cursor += strlen(cursor) + 1u;
    }
    for (uint32_t i = 0; i < head.envc; ++i) {
        k->envp[i] = (char *)cursor;
        cursor += strlen(cursor) + 1u;
    }
    k->program.cwd = *cursor ? cursor : NULL;
    for (uint32_t i = 0; i < head.fdc; ++i) {
        int32_t target = 0;
        memcpy(&target, k->body + targets_at + (size_t)i * sizeof(int32_t), sizeof(target));
        k->fds[i].source = OWNED_FD_SOURCES + (int)i;
        k->fds[i].target = target;
    }
    k->program.argv = k->argv;
    k->program.envp = k->envp;
    k->program.fds = k->fds;
    k->program.fd_count = head.fdc;
    k->program.flags = MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP;
    k->flags = head.flags;
    k->heartbeat_ms = head.heartbeat_ms ? head.heartbeat_ms : 1000u;
    k->observe_ms = head.observe_ms ? head.observe_ms : 2000u;
    k->linger_ms = head.linger_ms ? head.linger_ms : 60000u;
    if (head.start_deadline_ms) k->start_deadline = head.start_deadline_ms;
    maelys_sys_process_identity_t owner;
    owner.pid = (pid_t)head.owner_pid;
    owner.birth = head.owner_birth;
    owner.boot = head.owner_boot;
    maelys_sys_result_t watched = maelys_sys_process_exit_fd_open(&owner, &k->owner_fd);
    if (watched == MAELYS_SYS_ERR_NOT_FOUND) return ESRCH;
    if (watched != MAELYS_SYS_OK) return errno ? errno : EIO;
    return 0;
}

/* The barrier: the program is started as the leader of its new group. On
 * Linux the exec report is awaited in the loop, never here. */
static void start_program(keeper_t *k, uint64_t now) {
    if (k->aborted || k->owner_lost) return;
    pid_t pid = -1;
    int report = -1;
    maelys_sys_result_t started = maelys_sys__process_start(&k->program, &pid, &report);
    trace("start of %s: result %d, errno %d, pid %ld, report %d", k->program.path, (int)started, errno, (long)pid, report);
    if (started != MAELYS_SYS_OK) {
        owned_record_t record = make_record(OWNED_K_EXEC_FAILED);
        record.value = errno;
        enqueue(k, record);
        k->aborted = 1;
        finish(k, MAELYS_SYS_OWNED_START_FAILED, now);
        return;
    }
    k->x = pid;
    k->x_created = 1;
    k->report_fd = report;
    if (report < 0) {
        k->exec_confirmed = 1;
        for (size_t i = 0; i < k->program.fd_count; ++i) (void)close(k->fds[i].source);
        k->phase = MAELYS_SYS_OWNED_RUNNING;
        owned_record_t record = make_record(OWNED_K_STARTED);
        record.pid = (int32_t)k->x;
        enqueue(k, record);
        k->next_beat = now + k->heartbeat_ms;
    } else {
        set_nonblocking(report);
    }
}

/* The exec report, once readable: 0 bytes is the barrier passed, an int
 * is the errno of the refused exec, after which the child ends by itself
 * and is reaped WNOHANG in the loop. */
static void read_report(keeper_t *k, uint64_t now) {
    int error = 0;
    ssize_t got = read(k->report_fd, &error, sizeof(error));
    if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    (void)close(k->report_fd);
    k->report_fd = -1;
    if (got == 0) {
        k->exec_confirmed = 1;
        for (size_t i = 0; i < k->program.fd_count; ++i) (void)close(k->fds[i].source);
        if (k->owner_lost || k->aborted) {
            /* Lost while the child was before its exec: killed then by
             * number; the group now exists and is killed as a group. */
            if (!k->killed) kill_group(k, now);
            return;
        }
        k->phase = MAELYS_SYS_OWNED_RUNNING;
        owned_record_t record = make_record(OWNED_K_STARTED);
        record.pid = (int32_t)k->x;
        enqueue(k, record);
        k->next_beat = now + k->heartbeat_ms;
        return;
    }
    /* Refused: the child never ran the program. */
    owned_record_t record = make_record(OWNED_K_EXEC_FAILED);
    record.value = got == (ssize_t)sizeof(error) ? error : EIO;
    enqueue(k, record);
    k->aborted = 1;
    k->killed = 1; /* nothing to signal: the child ends on its own */
    k->exec_confirmed = 1;
    k->leader_ended = 1;
    k->phase = MAELYS_SYS_OWNED_RECLAIMING;
    k->observe_deadline = now + k->observe_ms;
    k->next_probe = now;
    k->probe_interval = 1;
}

/* --- requests ----------------------------------------------------------- */

static int remembered(const keeper_t *k, uint32_t seq, int32_t *out_value) {
    for (size_t i = 0; i < k->results.count; ++i) {
        if (k->results.seq[i] == seq) {
            *out_value = k->results.value[i];
            return 1;
        }
    }
    return 0;
}

static void remember(keeper_t *k, uint32_t seq, int32_t value) {
    k->results.seq[k->results.next] = seq;
    k->results.value[k->results.next] = value;
    k->results.next = (k->results.next + 1u) % OWNED_RESULTS_KEPT;
    if (k->results.count < OWNED_RESULTS_KEPT) ++k->results.count;
    if (!k->have_seq || seq > k->highest_seq) k->highest_seq = seq;
    k->have_seq = 1;
}

static void request_stop(keeper_t *k, uint32_t seq, uint64_t grace_ms, uint64_t now) {
    if (k->stopping || k->killed) {
        send_ack(k, seq, 0);
        return;
    }
    k->stopping = 1;
    k->stop_seq = seq;
    k->have_stop_seq = 1;
    if (!k->x_created) {
        k->aborted = 1;
        send_ack(k, seq, 0);
        finish(k, MAELYS_SYS_OWNED_START_FAILED, now);
        return;
    }
    if (k->leader_ended || !k->exec_confirmed) {
        send_ack(k, seq, 0);
        kill_group(k, now);
        return;
    }
    int result = kill(-k->x, SIGTERM);
    int error = result == 0 ? 0 : errno;
    note_kill(k, result);
    send_ack(k, seq, error);
    k->phase = MAELYS_SYS_OWNED_STOPPING;
    k->grace_deadline = now + grace_ms;
    send_phase(k);
}

static void handle_request(keeper_t *k, const owned_record_t *record, uint64_t now) {
    int32_t value = 0;
    trace("request type %u, seq %u, length %llu", (unsigned)record->type, record->seq, (unsigned long long)record->length);
    if (record->type == OWNED_H_DONE_ACK) {
        k->done_acked = 1;
        return;
    }
    if (record->type == OWNED_H_GO) {
        if (k->body || k->body_length) return; /* once */
        if (record->length < sizeof(owned_go_head_t) || record->length > OWNED_BODY_MAX) {
            owned_record_t lost = make_record(OWNED_K_LOST);
            lost.value = EINVAL;
            enqueue(k, lost);
            k->aborted = 1;
            finish(k, MAELYS_SYS_OWNED_START_FAILED, now);
            return;
        }
        k->body = malloc((size_t)record->length);
        if (!k->body) {
            owned_record_t lost = make_record(OWNED_K_LOST);
            lost.value = ENOMEM;
            enqueue(k, lost);
            k->aborted = 1;
            finish(k, MAELYS_SYS_OWNED_START_FAILED, now);
            return;
        }
        k->body_length = (size_t)record->length;
        k->body_got = 0;
        k->awaiting_body = 1;
        return;
    }
    /* Requests with a sequence: deduplicated, never run twice. */
    if (remembered(k, record->seq, &value)) {
        send_ack(k, record->seq, value);
        return;
    }
    if (k->have_seq && record->seq <= k->highest_seq) {
        send_ack(k, record->seq, EALREADY);
        return;
    }
    switch (record->type) {
    case OWNED_H_SIGNAL: {
        int error;
        if (!k->x_created || k->leader_reaped || !k->exec_confirmed) {
            error = ESRCH;
        } else {
            int result = kill(record->a ? -k->x : k->x, record->value);
            error = result == 0 ? 0 : errno;
            if (record->a) note_kill(k, result);
        }
        remember(k, record->seq, error);
        send_ack(k, record->seq, error);
        return;
    }
    case OWNED_H_STOP:
        remember(k, record->seq, 0);
        request_stop(k, record->seq, record->length, now);
        return;
    case OWNED_H_ABORT:
        remember(k, record->seq, 0);
        send_ack(k, record->seq, 0);
        if (!k->x_created) {
            k->aborted = 1;
            finish(k, MAELYS_SYS_OWNED_START_FAILED, now);
        } else if (!k->killed) {
            k->stopping = 1;
            kill_group(k, now);
        }
        return;
    case OWNED_H_TRANSFER:
        remember(k, record->seq, ENOTSUP);
        send_ack(k, record->seq, ENOTSUP);
        return;
    default:
        remember(k, record->seq, EINVAL);
        send_ack(k, record->seq, EINVAL);
        return;
    }
}

static void read_control(keeper_t *k, uint64_t now) {
    for (;;) {
        if (k->awaiting_body) {
            ssize_t got = read(k->ctl, k->body + k->body_got, k->body_length - k->body_got);
            if (got < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                k->ctl_closed = 1;
                return;
            }
            if (got == 0) {
                k->ctl_closed = 1;
                return;
            }
            k->body_got += (size_t)got;
            if (k->body_got < k->body_length) continue;
            k->awaiting_body = 0;
            int error = parse_body(k);
            trace("body of %zu bytes parsed: %d", k->body_length, error);
            if (error) {
                owned_record_t lost = make_record(OWNED_K_LOST);
                lost.value = error;
                enqueue(k, lost);
                k->aborted = 1;
                finish(k, MAELYS_SYS_OWNED_START_FAILED, now);
                return;
            }
            k->armed = 1;
            enqueue(k, make_record(OWNED_K_ARMED));
            start_program(k, now);
            continue;
        }
        /* The room left is written so that a fortified read can see it
         * is bounded by the record. */
        size_t at = k->rlen < OWNED_RECORD_SIZE ? k->rlen : 0u;
        if (k->rlen == OWNED_RECORD_SIZE) {
            /* A whole request, kept until its answer has a place in the
             * queue: a request is never run without room for its ACK and
             * the PHASE that may follow. Until then nothing more is read. */
            if (k->queue_count + 2u > QUEUE_SIZE) return;
            k->rlen = 0;
            owned_record_t record;
            memcpy(&record, k->rbuf, sizeof(record));
            if (record.magic != OWNED_MAGIC || record.protocol != OWNED_PROTOCOL) {
                k->ctl_closed = 1;
                return;
            }
            handle_request(k, &record, now);
            continue;
        }
        ssize_t got = read(k->ctl, k->rbuf + at, OWNED_RECORD_SIZE - at);
        if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            k->ctl_closed = 1;
            return;
        }
        if (got == 0) {
            k->ctl_closed = 1;
            return;
        }
        k->rlen = at + (size_t)got;
    }
}

/* --- signals ------------------------------------------------------------ */

static int open_signals(void) {
    sigset_t every;
    (void)sigfillset(&every);
    (void)sigdelset(&every, SIGKILL);
    (void)sigdelset(&every, SIGSTOP);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    (void)sigemptyset(&action.sa_mask);
    (void)sigaction(SIGCHLD, &action, NULL);
    if (sigprocmask(SIG_SETMASK, &every, NULL) != 0) return -1;
#if defined(__linux__)
    return signalfd(-1, &every, SFD_NONBLOCK | SFD_CLOEXEC);
#else
    int queue = kqueue();
    if (queue < 0) return -1;
    static const int watched[] = {SIGCHLD, SIGTERM, SIGINT, SIGHUP, SIGQUIT};
    for (size_t i = 0; i < sizeof(watched) / sizeof(watched[0]); ++i) {
        struct kevent change;
        EV_SET(&change, (uintptr_t)watched[i], EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
        if (kevent(queue, &change, 1, NULL, 0, NULL) != 0) {
            (void)close(queue);
            return -1;
        }
    }
    return queue;
#endif
}

/* Reads what arrived: a child's change, or a request to the keeper itself
 * to end, which is a loss of the owner. */
static void read_signals(keeper_t *k, uint64_t now) {
    int chld = 0, end = 0;
#if defined(__linux__)
    for (;;) {
        struct signalfd_siginfo info;
        ssize_t got = read(k->sigfd, &info, sizeof(info));
        if (got != (ssize_t)sizeof(info)) break;
        if (info.ssi_signo == SIGCHLD) chld = 1;
        else if (info.ssi_signo == SIGTERM || info.ssi_signo == SIGINT || info.ssi_signo == SIGHUP ||
            info.ssi_signo == SIGQUIT) end = 1;
    }
#else
    for (;;) {
        struct kevent event;
        struct timespec zero = {0, 0};
        int got = kevent(k->sigfd, NULL, 0, &event, 1, &zero);
        if (got <= 0) break;
        if (event.ident == SIGCHLD) chld = 1;
        else end = 1;
    }
#endif
    if (chld) {
        look_at_leader(k, now);
        reclaim(k, now);
    }
    if (end) lose(k, EINTR, now);
}

/* --- the loop ------------------------------------------------------------ */

static uint64_t earliest(uint64_t a, uint64_t b) {
    if (!a) return b;
    if (!b) return a;
    return a < b ? a : b;
}

static int exit_code(const keeper_t *k) {
    if (k->failure && k->killed && k->owner_lost == 0 && k->stopping == 0) return OWNED_EXIT_FAILED_CLOSED;
    switch (k->phase) {
    case MAELYS_SYS_OWNED_ENDED: return OWNED_EXIT_ENDED;
    case MAELYS_SYS_OWNED_START_FAILED: return OWNED_EXIT_NOT_STARTED;
    default: return OWNED_EXIT_UNCERTAIN;
    }
}

int main(int argc, char **argv) {
    keeper_t k;
    memset(&k, 0, sizeof(k));
    k.life = OWNED_FD_LIFE;
    k.ctl = OWNED_FD_CONTROL;
    k.owner_fd = -1;
    k.report_fd = -1;
    k.phase = MAELYS_SYS_OWNED_STARTING;
    uint64_t start_timeout = 10000u;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--start-timeout-ms") == 0) start_timeout = strtoull(argv[i + 1], NULL, 10);
    }
    if (fcntl(k.life, F_GETFD) < 0 || fcntl(k.ctl, F_GETFD) < 0) return 64;
    set_nonblocking(k.ctl);
    set_nonblocking(k.life);
#if defined(SO_NOSIGPIPE)
    {
        int one = 1;
        (void)setsockopt(k.ctl, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    k.sigfd = open_signals();
    if (k.sigfd < 0) return 65;
    uint64_t now = now_ms();
    k.start_deadline = now + start_timeout;
    k.observe_ms = 2000u;
    k.linger_ms = 60000u;
    k.heartbeat_ms = 1000u;
    owned_record_t hello = make_record(OWNED_K_HELLO);
    hello.a = OWNED_PROTOCOL;
    enqueue(&k, hello);

    for (;;) {
        now = now_ms();
        /* deadlines */
        if (!k.started_written && !k.aborted && !k.owner_lost && k.start_deadline && now >= k.start_deadline) {
            /* Not validated in time, STARTED queued but not fully written
             * included: what exists is stopped, nothing more is started.
             * NOT_STARTED says unvalidated, not unrun. */
            k.aborted = 1;
            k.start_deadline = 0;
            if (k.x_created && !k.killed) kill_group(&k, now);
            else if (!k.x_created) finish(&k, MAELYS_SYS_OWNED_START_FAILED, now);
        }
        if (k.stopping && k.phase == MAELYS_SYS_OWNED_STOPPING && k.grace_deadline && now >= k.grace_deadline) {
            kill_group(&k, now);
        }
        if (k.killed) reclaim(&k, now);
        if (k.phase == MAELYS_SYS_OWNED_RUNNING || k.phase == MAELYS_SYS_OWNED_LEADER_ENDED ||
            k.phase == MAELYS_SYS_OWNED_STOPPING) {
            if (k.next_beat && now >= k.next_beat) {
                enqueue(&k, make_record(OWNED_K_BEAT));
                k.next_beat = now + k.heartbeat_ms;
            }
        }
        if (flush(&k) < 0 && !k.x_created) {
            /* The host is gone before any start: nothing to protect. */
            k.aborted = 1;
            k.phase = MAELYS_SYS_OWNED_START_FAILED;
            k.done_queued = 2;
        }
        if (k.done_queued == 2 && (k.done_acked || k.ctl_closed || now >= k.done_deadline)) break;
        if (k.done_queued == 1 && (k.ctl_closed || now >= k.done_deadline)) break;

        /* what to wait for */
        struct pollfd fds[5];
        int count = 0;
        int at_ctl = -1, at_life = -1, at_owner = -1, at_sig = -1, at_report = -1;
        if (!k.ctl_closed) {
            int held = k.rlen == OWNED_RECORD_SIZE && k.queue_count + 2u > QUEUE_SIZE;
            at_ctl = count;
            fds[count].fd = k.ctl;
            fds[count].events = (short)((held ? 0 : POLLIN) | (k.queue_count ? POLLOUT : 0));
            fds[count].revents = 0;
            ++count;
        }
        if (!k.owner_lost) {
            at_life = count;
            fds[count].fd = k.life;
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
            if (k.owner_fd >= 0) {
                at_owner = count;
                fds[count].fd = k.owner_fd;
                fds[count].events = POLLIN;
                fds[count].revents = 0;
                ++count;
            }
        }
        at_sig = count;
        fds[count].fd = k.sigfd;
        fds[count].events = POLLIN;
        fds[count].revents = 0;
        ++count;
        if (k.report_fd >= 0) {
            at_report = count;
            fds[count].fd = k.report_fd;
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
        }
        /* The deadlines of the phase at hand, and no other: a passed one
         * that no longer matters would make poll return at once. */
        uint64_t next = 0;
        if (!k.started_written && !k.aborted) next = earliest(next, k.start_deadline);
        switch (k.phase) {
        case MAELYS_SYS_OWNED_STOPPING:
            next = earliest(next, k.grace_deadline);
            break;
        case MAELYS_SYS_OWNED_RECLAIMING:
            next = earliest(next, earliest(k.observe_deadline, now + 10u));
            break;
        case MAELYS_SYS_OWNED_OBSERVING:
            next = earliest(next, earliest(k.next_probe, k.observe_deadline));
            break;
        case MAELYS_SYS_OWNED_UNCERTAIN:
            /* next_probe paces both the probes and, while the leader is
             * not reaped, the attempts to reap it */
            next = earliest(next, earliest(k.next_probe, k.linger_deadline));
            break;
        default:
            break;
        }
        if (k.next_beat && !k.killed) next = earliest(next, k.next_beat);
        if (k.done_queued) next = earliest(next, k.done_deadline);
        int timeout = -1;
        if (next) timeout = next > now ? (int)(next - now > 60000u ? 60000u : next - now) : 0;
        int ready = poll(fds, (nfds_t)count, timeout);
        now = now_ms();
        trace("poll: ready %d (ctl %x life %x owner %x sig %x report %x), phase %d", ready,
            at_ctl >= 0 ? fds[at_ctl].revents : 0, at_life >= 0 ? fds[at_life].revents : 0,
            at_owner >= 0 ? fds[at_owner].revents : 0, at_sig >= 0 ? fds[at_sig].revents : 0,
            at_report >= 0 ? fds[at_report].revents : 0, (int)k.phase);
        if (ready < 0) {
            if (errno == EINTR) continue;
            fail(&k, errno, now);
            continue;
        }
        if (at_report >= 0 && fds[at_report].revents) read_report(&k, now);
        if (at_sig >= 0 && fds[at_sig].revents) read_signals(&k, now);
        if (at_owner >= 0 && fds[at_owner].revents) lose(&k, ESRCH, now);
        if (at_life >= 0 && fds[at_life].revents) {
            char byte;
            ssize_t got = read(k.life, &byte, 1);
            if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                lose(&k, EPIPE, now);
            }
        }
        if (at_ctl >= 0 && (fds[at_ctl].revents & (POLLIN | POLLHUP | POLLERR))) read_control(&k, now);
        if (at_ctl >= 0 && (fds[at_ctl].revents & POLLOUT)) (void)flush(&k);
        if (k.exec_confirmed && !k.leader_ended) look_at_leader(&k, now);
    }
    (void)flush(&k);
    trace("exit %d: phase %d, aborted %d, owner lost %d, x %ld", exit_code(&k), (int)k.phase, k.aborted,
        k.owner_lost, (long)k.x);
    _exit(exit_code(&k));
}
