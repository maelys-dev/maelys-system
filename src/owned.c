/*
 * Owned process groups, the host's side: start the keeper without a
 * blocking read, hand it the program, follow what it reports, and never
 * signal the group or the keeper by number once the start has returned.
 *
 * The channel is a stream socket carrying records of 64 bytes; the host
 * keeps a read cursor for a record in progress and a write cursor for a
 * request that did not go in one piece. When the keeper's end descriptor
 * says it has gone, what it wrote before is read first, without waiting
 * for an EOF that a copy of the socket elsewhere may hold back.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "maelys/sys/owned.h"

#include "maelys/sys/clock.h"
#include "maelys/sys/process.h"
#include "owned_protocol.h"
#include "process_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAELYS_SYS_KEEPER_PATH
#define MAELYS_SYS_KEEPER_PATH "/usr/local/libexec/maelys-sys/keeper"
#endif

extern char **environ;

/* A step a white-box test may act at; nothing in an ordinary build. */
#if defined(MAELYS_SYS_OWNED_TESTING)
static int owned_fault(const char *step);
#define OWNED_FAULT(step) (owned_fault(step) != 0)
#else
#define OWNED_FAULT(step) 0
#endif

#define ISSUED_KEPT 32u

typedef struct issued {
    uint32_t seq;
    int answered;
    int value;
} issued_t;

struct maelys_sys_owned {
    pid_t keeper;
    int keeper_fd;
    int ctl;
    int life;
    atomic_int x;        /* read by owned_pid from any thread */
    int started_seen;    /* STARTED read: the keeper validated the start */
    int negotiated;      /* HELLO read with this protocol */
    maelys_sys_owned_phase_t phase;
    maelys_sys_owned_state_t state;
    int final;
    int done_seen;
    int keeper_raw;
    int timed_out;
    uint64_t heartbeat_ms;
    uint64_t last_beat;
    /* receiving */
    uint8_t rbuf[OWNED_RECORD_SIZE];
    size_t rlen;
    /* sending */
    owned_record_t pending[OWNED_PENDING_MAX];
    size_t pending_head, pending_count, pending_sent;
    uint32_t next_seq;
    uint32_t stop_seq;
    int have_stop;
    issued_t issued[ISSUED_KEPT];
    size_t issued_next;
    size_t outstanding; /* issued and not yet answered: at most OWNED_PENDING_MAX */
};

static uint64_t now_ms(void) {
    uint64_t now = 0;
    (void)maelys_sys_monotonic_ms(&now);
    return now;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static owned_record_t make_record(uint16_t type) {
    owned_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = OWNED_MAGIC;
    record.protocol = OWNED_PROTOCOL;
    record.type = type;
    return record;
}

/* --- the channel ------------------------------------------------------ */

static int flush(maelys_sys_owned_t *owned) {
    while (owned->pending_count && owned->ctl >= 0) {
        const uint8_t *bytes = (const uint8_t *)&owned->pending[owned->pending_head];
        ssize_t sent = send(owned->ctl, bytes + owned->pending_sent, OWNED_RECORD_SIZE - owned->pending_sent,
#if defined(MSG_NOSIGNAL)
            MSG_NOSIGNAL
#else
            0
#endif
        );
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        owned->pending_sent += (size_t)sent;
        if (owned->pending_sent == OWNED_RECORD_SIZE) {
            owned->pending_head = (owned->pending_head + 1u) % OWNED_PENDING_MAX;
            --owned->pending_count;
            owned->pending_sent = 0;
        }
    }
    return 0;
}

static int enqueue(maelys_sys_owned_t *owned, owned_record_t record) {
    if (owned->pending_count == OWNED_PENDING_MAX) return -1;
    owned->pending[(owned->pending_head + owned->pending_count) % OWNED_PENDING_MAX] = record;
    ++owned->pending_count;
    (void)flush(owned);
    return 0;
}

static void issue(maelys_sys_owned_t *owned, uint32_t seq) {
    if (owned->issued[owned->issued_next].seq && !owned->issued[owned->issued_next].answered &&
        owned->outstanding) {
        --owned->outstanding; /* evicted unanswered: cannot happen within the limit */
    }
    ++owned->outstanding;
    owned->issued[owned->issued_next].seq = seq;
    owned->issued[owned->issued_next].answered = 0;
    owned->issued[owned->issued_next].value = 0;
    owned->issued_next = (owned->issued_next + 1u) % ISSUED_KEPT;
}

static void answered(maelys_sys_owned_t *owned, uint32_t seq, int value) {
    for (size_t i = 0; i < ISSUED_KEPT; ++i) {
        if (owned->issued[i].seq == seq && owned->issued[i].seq) {
            if (!owned->issued[i].answered && owned->outstanding) --owned->outstanding;
            owned->issued[i].answered = 1;
            owned->issued[i].value = value;
            return;
        }
    }
}

static void fill_leader(maelys_sys_owned_t *owned, const owned_record_t *record) {
    owned->state.leader_ended = 1;
    memset(&owned->state.leader, 0, sizeof(owned->state.leader));
    if (record->a) {
        owned->state.leader.exited = 1;
        owned->state.leader.exit_code = (int)record->b;
    } else if (record->value) {
        owned->state.leader.signaled = 1;
        owned->state.leader.term_signal = (int)record->b;
    }
}

static void apply_flags(maelys_sys_owned_t *owned, uint32_t flags, int failure) {
    owned->state.leader_unreaped = !!(flags & OWNED_F_LEADER_UNREAPED);
    owned->state.last_signal_refused = !!(flags & OWNED_F_SIGNAL_REFUSED);
    owned->state.members_present = !!(flags & OWNED_F_MEMBERS_PRESENT);
    if (failure && !owned->state.keeper_failure) owned->state.keeper_failure = failure;
}

/* One record from the keeper, applied to the state. */
static void apply(maelys_sys_owned_t *owned, const owned_record_t *record, uint64_t now) {
    switch (record->type) {
    case OWNED_K_BEAT:
        owned->last_beat = now;
        owned->state.keeper_unresponsive = 0;
        break;
    case OWNED_K_STARTED:
        atomic_store(&owned->x, record->pid);
        owned->started_seen = 1;
        owned->last_beat = now;
        if (owned->timed_out && owned->phase == MAELYS_SYS_OWNED_START_FAILED) {
            owned->state.started_after_timeout = 1;
        }
        if (!owned->final) owned->phase = MAELYS_SYS_OWNED_RUNNING;
        break;
    case OWNED_K_EXITED:
        fill_leader(owned, record);
        break;
    case OWNED_K_PHASE:
        if (!owned->final && !(owned->timed_out && record->a == (uint32_t)MAELYS_SYS_OWNED_STARTING)) {
            owned->phase = (maelys_sys_owned_phase_t)record->a;
        }
        if (record->pid > 0) atomic_store(&owned->x, record->pid);
        apply_flags(owned, record->b, record->value);
        break;
    case OWNED_K_ACK:
        answered(owned, record->seq, record->value);
        break;
    case OWNED_K_DONE:
        owned->done_seen = 1;
        owned->final = 1;
        owned->phase = (maelys_sys_owned_phase_t)record->a;
        apply_flags(owned, record->b, record->value);
        if (record->pid > 0) atomic_store(&owned->x, record->pid);
        {
            owned_record_t ack = make_record(OWNED_H_DONE_ACK);
            (void)enqueue(owned, ack);
        }
        break;
    case OWNED_K_LOST:
    case OWNED_K_EXEC_FAILED:
    case OWNED_K_HELLO:
    case OWNED_K_ARMED:
    default:
        break;
    }
}

/* Reads one complete record if the socket holds one, without blocking,
 * and applies it: 1 got, 0 none yet, -1 the socket reported its end or
 * carried something that is not a record. */
static int read_one(maelys_sys_owned_t *owned, uint64_t now, owned_record_t *out) {
    while (owned->ctl >= 0) {
        size_t at = owned->rlen % OWNED_RECORD_SIZE;
        ssize_t got = recv(owned->ctl, owned->rbuf + at, OWNED_RECORD_SIZE - at, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (got == 0) return -1;
        owned->rlen = at + (size_t)got;
        if (owned->rlen < OWNED_RECORD_SIZE) continue;
        owned->rlen = 0;
        memcpy(out, owned->rbuf, sizeof(*out));
        /* The magic says it is a keeper; the protocol is judged at HELLO,
         * so that another protocol is an incompatibility, not a closed
         * channel. After HELLO, another number is a corrupt channel. */
        if (out->magic != OWNED_MAGIC) return -1;
        if (owned->negotiated && out->protocol != OWNED_PROTOCOL) return -1;
        apply(owned, out, now);
        return 1;
    }
    return -1;
}

/* Reads every complete record available, without blocking: 1 when the
 * socket reported its end, 0 otherwise. */
static int drain(maelys_sys_owned_t *owned, uint64_t now) {
    for (;;) {
        owned_record_t record;
        int got = read_one(owned, now, &record);
        if (got == 0) return 0;
        if (got < 0) return 1;
    }
}

/* Whether the keeper's end descriptor is readable now. */
static int keeper_ended(const maelys_sys_owned_t *owned) {
    if (owned->keeper_fd < 0) return 0;
    struct pollfd fd = {owned->keeper_fd, POLLIN, 0};
    int ready;
    do {
        ready = poll(&fd, 1, 0);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

static maelys_sys_owned_phase_t decode_exit(maelys_sys_owned_t *owned) {
    if (!WIFEXITED(owned->keeper_raw)) return MAELYS_SYS_OWNED_KEEPER_LOST;
    switch (WEXITSTATUS(owned->keeper_raw)) {
    case OWNED_EXIT_ENDED: return MAELYS_SYS_OWNED_ENDED;
    case OWNED_EXIT_UNCERTAIN: return MAELYS_SYS_OWNED_UNCERTAIN_FINAL;
    case OWNED_EXIT_NOT_STARTED: return MAELYS_SYS_OWNED_START_FAILED;
    case OWNED_EXIT_FAILED_CLOSED:
        if (!owned->state.keeper_failure) owned->state.keeper_failure = EIO;
        return MAELYS_SYS_OWNED_UNCERTAIN_FINAL;
    default: return MAELYS_SYS_OWNED_KEEPER_LOST;
    }
}

/* Tries to reap the keeper once; sets what the state says of it. */
static void reap_keeper(maelys_sys_owned_t *owned) {
    if (owned->state.keeper_reaped) return;
    int raw = 0;
    pid_t got;
    do {
        got = waitpid(owned->keeper, &raw, WNOHANG);
    } while (got < 0 && errno == EINTR);
    if (got == owned->keeper) {
        owned->keeper_raw = raw;
        owned->state.keeper_reaped = 1;
    } else if (got < 0 && errno == ECHILD) {
        owned->state.keeper_reaped = 1;
        owned->state.keeper_reaped_elsewhere = 1;
    }
}

/* Everything poll does, without waiting. */
static void refresh(maelys_sys_owned_t *owned) {
    uint64_t now = now_ms();
    if (!owned->state.keeper_reaped) {
        (void)flush(owned);
        int eof = drain(owned, now);
        if (keeper_ended(owned) || eof) {
            /* What it wrote before is read first; a partial record is
             * dropped; the end is concluded without an EOF. */
            (void)drain(owned, now);
            if (owned->rlen) {
                owned->state.truncated_final = 1;
                owned->rlen = 0;
            }
            reap_keeper(owned);
            if (keeper_ended(owned) && !owned->state.keeper_reaped) reap_keeper(owned);
            if (owned->state.keeper_reaped && !owned->final) {
                owned->final = 1;
                if (owned->done_seen) {
                    /* the phase of DONE stands */
                } else if (!owned->state.keeper_reaped_elsewhere) {
                    owned->phase = decode_exit(owned);
                    owned->state.state_from_exit_code = owned->phase != MAELYS_SYS_OWNED_KEEPER_LOST;
                } else {
                    owned->phase = MAELYS_SYS_OWNED_KEEPER_LOST;
                }
            }
        }
    }
    int active = owned->phase == MAELYS_SYS_OWNED_RUNNING || owned->phase == MAELYS_SYS_OWNED_LEADER_ENDED ||
        owned->phase == MAELYS_SYS_OWNED_STOPPING;
    if (active && !owned->final && owned->last_beat) {
        uint64_t limit = owned->last_beat + 2u * owned->heartbeat_ms;
        owned->state.keeper_unresponsive = now > limit;
        owned->state.next_deadline_ms = owned->state.keeper_unresponsive ? now + owned->heartbeat_ms : limit;
    } else {
        owned->state.next_deadline_ms = MAELYS_SYS_DEADLINE_INFINITE;
    }
    owned->state.want_write = owned->pending_count > 0;
    owned->state.phase = owned->phase;
}

/* --- the start --------------------------------------------------------- */

static maelys_sys_result_t check_sigchld(void) {
    struct sigaction current;
    if (sigaction(SIGCHLD, NULL, &current) != 0) return MAELYS_SYS_ERR_OS;
    if (current.sa_handler == SIG_IGN || (current.sa_flags & SA_NOCLDWAIT)) return MAELYS_SYS_ERR_STATE;
    return MAELYS_SYS_OK;
}

static size_t strings_length(const maelys_sys_process_options_t *program, char *const *envp,
    size_t *out_argc, size_t *out_envc) {
    size_t length = strlen(program->path) + 1u;
    size_t argc = 0, envc = 0;
    for (; program->argv[argc]; ++argc) length += strlen(program->argv[argc]) + 1u;
    for (; envp[envc]; ++envc) length += strlen(envp[envc]) + 1u;
    length += (program->cwd ? strlen(program->cwd) : 0u) + 1u;
    *out_argc = argc;
    *out_envc = envc;
    return length;
}

static char *put(char *cursor, const char *text) {
    size_t length = strlen(text) + 1u;
    memcpy(cursor, text, length);
    return cursor + length;
}

static maelys_sys_result_t build_body(const maelys_sys_owned_options_t *options,
    const maelys_sys_process_identity_t *owner, char **out_body, size_t *out_length) {
    const maelys_sys_process_options_t *program = options->program;
    char *const *envp = program->envp ? program->envp : environ;
    size_t argc = 0, envc = 0;
    size_t strings = strings_length(program, envp, &argc, &envc);
    if (argc > 65536u || envc > 65536u) return MAELYS_SYS_ERR_ARGUMENT;
    size_t length = sizeof(owned_go_head_t) + strings + program->fd_count * sizeof(int32_t);
    if (length > OWNED_BODY_MAX) return MAELYS_SYS_ERR_ARGUMENT;
    char *body = malloc(length);
    if (!body) return MAELYS_SYS_ERR_MEMORY;
    owned_go_head_t head;
    memset(&head, 0, sizeof(head));
    head.argc = (uint32_t)argc;
    head.envc = (uint32_t)envc;
    head.fdc = (uint32_t)program->fd_count;
    head.flags = options->flags;
    head.owner_pid = (int32_t)owner->pid;
    head.owner_birth = owner->birth;
    head.owner_boot = owner->boot;
    head.heartbeat_ms = options->heartbeat_ms;
    head.observe_ms = options->observe_ms;
    head.linger_ms = options->linger_ms;
    head.start_deadline_ms = options->start_deadline_ms;
    head.strings_length = strings;
    memcpy(body, &head, sizeof(head));
    char *cursor = body + sizeof(head);
    cursor = put(cursor, program->path);
    for (size_t i = 0; i < argc; ++i) cursor = put(cursor, program->argv[i]);
    for (size_t i = 0; i < envc; ++i) cursor = put(cursor, envp[i]);
    cursor = put(cursor, program->cwd ? program->cwd : "");
    for (size_t i = 0; i < program->fd_count; ++i) {
        int32_t target = program->fds[i].target;
        memcpy(cursor, &target, sizeof(target));
        cursor += sizeof(target);
    }
    *out_body = body;
    *out_length = length;
    return MAELYS_SYS_OK;
}

/* Waits for the descriptor until the deadline: 1 ready, 0 timeout, -1 errno. */
static int wait_fd(int fd, short events, uint64_t deadline) {
    for (;;) {
        uint64_t now = now_ms();
        if (now >= deadline) return 0;
        uint64_t left = deadline - now;
        struct pollfd pfd = {fd, events, 0};
        int ready = poll(&pfd, 1, (int)(left > 60000u ? 60000u : left));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return -1;
        if (ready > 0) return 1;
    }
}

/* Writes everything, waiting as the deadline allows. */
static int send_all(int fd, const char *bytes, size_t length, uint64_t deadline) {
    size_t done = 0;
    while (done < length) {
        ssize_t sent = send(fd, bytes + done, length - done,
#if defined(MSG_NOSIGNAL)
            MSG_NOSIGNAL
#else
            0
#endif
        );
        if (sent < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int ready = wait_fd(fd, POLLOUT, deadline);
                if (ready <= 0) return ready == 0 ? -2 : -1;
                continue;
            }
            return -1;
        }
        done += (size_t)sent;
    }
    return 0;
}

/* Waits for one record of the wanted type (or any, 0), applying every
 * record read: 1 got, 0 deadline, -1 the keeper ended or the channel
 * closed, -2 the start was refused by the keeper (LOST or EXEC_FAILED,
 * copied to *out). */
static int await_record(maelys_sys_owned_t *owned, uint16_t wanted, uint64_t deadline, owned_record_t *out) {
    int ended = 0;
    for (;;) {
        uint64_t now = now_ms();
        owned_record_t record;
        int got = read_one(owned, now, &record);
        if (got < 0) return -1;
        if (got == 1) {
            if (record.type == OWNED_K_LOST || record.type == OWNED_K_EXEC_FAILED) {
                *out = record;
                return -2;
            }
            if (wanted == 0 || record.type == wanted) {
                *out = record;
                return 1;
            }
            continue;
        }
        /* Nothing more in the socket now. The keeper gone: what it wrote
         * was read above; a last look at the socket, then its end. */
        if (ended) return -1;
        if (keeper_ended(owned)) {
            ended = 1;
            continue;
        }
        if (now >= deadline) return 0;
        struct pollfd fds[2] = {{owned->ctl, POLLIN, 0}, {owned->keeper_fd, POLLIN, 0}};
        uint64_t left = deadline - now;
        int ready = poll(fds, owned->keeper_fd >= 0 ? 2 : 1, (int)(left > 60000u ? 60000u : left));
        if (ready < 0 && errno != EINTR) return -1;
    }
}

static void close_quietly(int *fd) {
    if (*fd >= 0) {
        int saved = errno;
        (void)close(*fd);
        errno = saved;
        *fd = -1;
    }
}

/* Lets the keeper end by itself (the life pipe closed), within the
 * deadline, and reaps it: 1 reaped, 0 still there. */
static int let_keeper_end(maelys_sys_owned_t *owned, uint64_t deadline) {
    close_quietly(&owned->life);
    for (;;) {
        reap_keeper(owned);
        if (owned->state.keeper_reaped) return 1;
        if (owned->keeper_fd < 0) return 0;
        int ready = wait_fd(owned->keeper_fd, POLLIN, deadline);
        if (ready <= 0) {
            reap_keeper(owned);
            return owned->state.keeper_reaped;
        }
        /* readable: ended; one more look reaps it */
        reap_keeper(owned);
        if (owned->state.keeper_reaped) return 1;
        uint64_t now = now_ms();
        if (now >= deadline) return 0;
        struct pollfd none = {-1, 0, 0};
        (void)poll(&none, 0, 1);
    }
}

static void destroy(maelys_sys_owned_t *owned) {
    close_quietly(&owned->ctl);
    close_quietly(&owned->life);
    close_quietly(&owned->keeper_fd);
    free(owned);
}

maelys_sys_result_t maelys_sys_owned_spawn(
    const maelys_sys_owned_options_t *options,
    maelys_sys_owned_t **out_owned) {
    if (out_owned) *out_owned = NULL;
    if (!out_owned || !options || !options->keeper_path || options->keeper_path[0] != '/' ||
        !options->program || !options->program->path || !options->program->argv) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    const maelys_sys_process_options_t *program = options->program;
    if (program->fd_count > MAELYS_SYS_OWNED_MAX_FDS || (program->fd_count && !program->fds)) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (program->flags & (MAELYS_SYS_PROCESS_NEW_SESSION | MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP)) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (options->flags & ~(unsigned)MAELYS_SYS_OWNED_END_WITH_LEADER) return MAELYS_SYS_ERR_ARGUMENT;
    if (options->start_deadline_ms == MAELYS_SYS_DEADLINE_INFINITE) return MAELYS_SYS_ERR_ARGUMENT;
    for (size_t i = 0; i < program->fd_count; ++i) {
        if (program->fds[i].source < 0 || program->fds[i].target < 0) return MAELYS_SYS_ERR_ARGUMENT;
        if (fcntl(program->fds[i].source, F_GETFD) < 0) return MAELYS_SYS_ERR_ARGUMENT;
        for (size_t j = 0; j < i; ++j) {
            if (program->fds[j].target == program->fds[i].target) return MAELYS_SYS_ERR_ARGUMENT;
        }
    }
    maelys_sys_result_t result = check_sigchld();
    if (result != MAELYS_SYS_OK) return result;

    maelys_sys_process_identity_t self;
    result = maelys_sys_process_identify(getpid(), &self);
    if (result != MAELYS_SYS_OK) return result;

    maelys_sys_owned_t *owned = calloc(1, sizeof(*owned));
    if (!owned) return MAELYS_SYS_ERR_MEMORY;
    owned->keeper_fd = -1;
    owned->ctl = -1;
    owned->life = -1;
    owned->phase = MAELYS_SYS_OWNED_STARTING;
    owned->next_seq = 1;
    owned->heartbeat_ms = options->heartbeat_ms ? options->heartbeat_ms : 1000u;

    char *body = NULL;
    size_t body_length = 0;
    result = build_body(options, &self, &body, &body_length);
    if (result != MAELYS_SYS_OK) {
        destroy(owned);
        return result;
    }

    int life[2] = {-1, -1};
    int ctl[2] = {-1, -1};
    if (pipe(life) != 0) {
        int saved = errno;
        free(body);
        destroy(owned);
        errno = saved;
        return MAELYS_SYS_ERR_OS;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ctl) != 0 ||
        fcntl(life[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(life[1], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(ctl[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(ctl[1], F_SETFD, FD_CLOEXEC) != 0 ||
        set_nonblocking(ctl[0]) != 0) {
        int saved = errno;
        close_quietly(&life[0]);
        close_quietly(&life[1]);
        close_quietly(&ctl[0]);
        close_quietly(&ctl[1]);
        free(body);
        destroy(owned);
        errno = saved;
        return MAELYS_SYS_ERR_OS;
    }
#if defined(SO_NOSIGPIPE)
    {
        int one = 1;
        (void)setsockopt(ctl[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    owned->life = life[1];
    owned->ctl = ctl[0];

    /* the keeper's own table: life, control, then the program's sources */
    maelys_sys_process_fd_t table[MAELYS_SYS_PROCESS_MAX_FDS];
    table[0].source = life[0];
    table[0].target = OWNED_FD_LIFE;
    table[1].source = ctl[1];
    table[1].target = OWNED_FD_CONTROL;
    for (size_t i = 0; i < program->fd_count; ++i) {
        table[2 + i].source = program->fds[i].source;
        table[2 + i].target = OWNED_FD_SOURCES + (int)i;
    }
    uint64_t now = now_ms();
    char timeout_text[32];
    snprintf(timeout_text, sizeof(timeout_text), "%llu",
        (unsigned long long)(options->start_deadline_ms > now ? options->start_deadline_ms - now : 0u));
    char *keeper_argv[] = {"maelys-sys-keeper", "--start-timeout-ms", timeout_text, NULL};
    char *keeper_envp[] = {"MAELYS_SYS_KEEPER=1", NULL};
    maelys_sys_process_options_t keeper_options;
    memset(&keeper_options, 0, sizeof(keeper_options));
    keeper_options.path = options->keeper_path;
    keeper_options.argv = keeper_argv;
    keeper_options.envp = keeper_envp;
    keeper_options.fds = table;
    keeper_options.fd_count = 2 + program->fd_count;
    keeper_options.flags = MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP;

    int report = -1;
    result = maelys_sys__process_start(&keeper_options, &owned->keeper, &report);
    close_quietly(&life[0]);
    close_quietly(&ctl[1]);
    if (result != MAELYS_SYS_OK) {
        int saved = errno;
        free(body);
        destroy(owned);
        errno = saved;
        return result == MAELYS_SYS_ERR_NOT_FOUND ? MAELYS_SYS_ERR_UNSUPPORTED : result;
    }
    uint64_t deadline = options->start_deadline_ms;
    if (report >= 0) {
        /* the exec report, awaited in poll: 0 bytes once the keeper runs */
        int ready = wait_fd(report, POLLIN, deadline);
        int error = 0;
        ssize_t got = -1;
        if (ready > 0) {
            do {
                got = read(report, &error, sizeof(error));
            } while (got < 0 && errno == EINTR);
        }
        close_quietly(&report);
        if (ready <= 0 || got != 0) {
            /* Not running in time, or its exec refused: it is this
             * call's own unreaped child, so its number is safe to kill. */
            if (ready <= 0) (void)kill(owned->keeper, SIGKILL);
            uint64_t until = now_ms() + 50u;
            for (;;) {
                reap_keeper(owned);
                if (owned->state.keeper_reaped || now_ms() >= until) break;
                struct pollfd none = {-1, 0, 0};
                (void)poll(&none, 0, 1);
            }
            free(body);
            if (ready > 0 && got == (ssize_t)sizeof(error)) {
                if (owned->state.keeper_reaped) {
                    destroy(owned);
                    errno = error;
                    return error == ENOENT ? MAELYS_SYS_ERR_UNSUPPORTED : MAELYS_SYS_ERR_OS;
                }
            }
            if (owned->state.keeper_reaped) {
                destroy(owned);
                return ready < 0 ? MAELYS_SYS_ERR_OS : MAELYS_SYS_ERR_TIMEOUT;
            }
            owned->phase = MAELYS_SYS_OWNED_START_FAILED;
            owned->final = 1;
            owned->timed_out = 1;
            *out_owned = owned;
            return ready < 0 ? MAELYS_SYS_ERR_OS : MAELYS_SYS_ERR_TIMEOUT;
        }
    }
    owned->keeper_fd = maelys_sys__process_exit_fd_open(owned->keeper);
    if (owned->keeper_fd < 0) {
        /* Cannot be followed: stopped here, while its number is still
         * this call's to signal. */
        int saved = errno;
        (void)kill(owned->keeper, SIGKILL);
        uint64_t until = now_ms() + 50u;
        for (;;) {
            reap_keeper(owned);
            if (owned->state.keeper_reaped || now_ms() >= until) break;
            struct pollfd none = {-1, 0, 0};
            (void)poll(&none, 0, 1);
        }
        free(body);
        if (owned->state.keeper_reaped) {
            destroy(owned);
            errno = saved;
            return saved == ENOSYS ? MAELYS_SYS_ERR_UNSUPPORTED : MAELYS_SYS_ERR_OS;
        }
        owned->phase = MAELYS_SYS_OWNED_START_FAILED;
        owned->final = 1;
        *out_owned = owned;
        errno = saved;
        return saved == ENOSYS ? MAELYS_SYS_ERR_UNSUPPORTED : MAELYS_SYS_ERR_OS;
    }

    /* HELLO, then GO and its body, then ARMED and STARTED */
    owned_record_t record;
    int got = await_record(owned, OWNED_K_HELLO, deadline, &record);
    if (got == 1 && record.a == OWNED_PROTOCOL && record.protocol == OWNED_PROTOCOL) owned->negotiated = 1;
    if (!owned->negotiated) {
        free(body);
        int reaped = let_keeper_end(owned, got == 0 ? now_ms() + 100u : deadline);
        if (reaped) {
            destroy(owned);
            return got == 1 ? MAELYS_SYS_ERR_UNSUPPORTED : got == 0 ? MAELYS_SYS_ERR_TIMEOUT : MAELYS_SYS_ERR_CLOSED;
        }
        owned->phase = got < 0 ? MAELYS_SYS_OWNED_KEEPER_LOST : MAELYS_SYS_OWNED_START_FAILED;
        owned->final = got < 0;
        owned->timed_out = got == 0;
        *out_owned = owned;
        return got == 1 ? MAELYS_SYS_ERR_UNSUPPORTED : got == 0 ? MAELYS_SYS_ERR_TIMEOUT : MAELYS_SYS_ERR_CLOSED;
    }
    owned_record_t go = make_record(OWNED_H_GO);
    go.length = body_length;
    int sent = send_all(owned->ctl, (const char *)&go, sizeof(go), deadline);
    if (sent == 0) sent = send_all(owned->ctl, body, body_length, deadline);
    free(body);
    if (sent != 0) {
        int reaped = let_keeper_end(owned, now_ms() + 100u);
        if (reaped) {
            destroy(owned);
            return sent == -2 ? MAELYS_SYS_ERR_TIMEOUT : MAELYS_SYS_ERR_CLOSED;
        }
        owned->phase = MAELYS_SYS_OWNED_START_FAILED;
        owned->timed_out = 1;
        *out_owned = owned;
        return sent == -2 ? MAELYS_SYS_ERR_TIMEOUT : MAELYS_SYS_ERR_CLOSED;
    }
    got = await_record(owned, OWNED_K_STARTED, deadline, &record);
    if (got == 1) {
        /* STARTED applied by the reader: the phase is RUNNING unless a
         * later record has already said otherwise. */
        owned->last_beat = now_ms();
        *out_owned = owned;
        return MAELYS_SYS_OK;
    }
    if (got == -2) {
        /* Refused by the keeper before any program ran: it leaves by
         * itself once its DONE is answered; the start's own error is
         * returned. */
        int error = record.value;
        (void)drain(owned, now_ms());
        (void)flush(owned);
        int reaped = let_keeper_end(owned, deadline);
        if (reaped) {
            destroy(owned);
            errno = error;
            return record.type == OWNED_K_EXEC_FAILED && error == ENOENT ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
        }
        owned->phase = MAELYS_SYS_OWNED_START_FAILED;
        *out_owned = owned;
        errno = error;
        return record.type == OWNED_K_EXEC_FAILED && error == ENOENT ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    }
    if (got == 0) {
        /* One last look, without waiting: a complete STARTED wins, and
         * what came after it stands: the start succeeded, the state is
         * whatever the keeper said last, a final phase included. */
        if (OWNED_FAULT("before-last-look")) {}
        (void)drain(owned, now_ms());
        if (owned->started_seen) {
            if (!owned->last_beat) owned->last_beat = now_ms();
            *out_owned = owned;
            return MAELYS_SYS_OK;
        }
        owned->timed_out = 1;
        owned->phase = MAELYS_SYS_OWNED_START_FAILED;
        owned_record_t abort = make_record(OWNED_H_ABORT);
        abort.seq = UINT32_MAX;
        issue(owned, abort.seq);
        (void)enqueue(owned, abort);
        close_quietly(&owned->life);
        *out_owned = owned;
        return MAELYS_SYS_ERR_TIMEOUT;
    }
    /* the keeper ended, or the channel closed, before STARTED */
    refresh(owned);
    if (!owned->final) {
        owned->final = 1;
        owned->phase = MAELYS_SYS_OWNED_KEEPER_LOST;
    }
    *out_owned = owned;
    return MAELYS_SYS_ERR_CLOSED;
}

/* --- following ----------------------------------------------------------- */

pid_t maelys_sys_owned_pid(const maelys_sys_owned_t *owned) {
    if (!owned) return -1;
    int x = atomic_load(&((maelys_sys_owned_t *)owned)->x);
    return x > 0 ? (pid_t)x : -1;
}

int maelys_sys_owned_fd(const maelys_sys_owned_t *owned) {
    return owned ? owned->ctl : -1;
}

int maelys_sys_owned_keeper_fd(const maelys_sys_owned_t *owned) {
    return owned ? owned->keeper_fd : -1;
}

maelys_sys_result_t maelys_sys_owned_poll(maelys_sys_owned_t *owned, maelys_sys_owned_state_t *out_state) {
    if (out_state) memset(out_state, 0, sizeof(*out_state));
    if (!owned || !out_state) return MAELYS_SYS_ERR_ARGUMENT;
    refresh(owned);
    *out_state = owned->state;
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t request(maelys_sys_owned_t *owned, owned_record_t record, int for_stop, uint32_t *out_seq) {
    if (owned->final && owned->state.keeper_reaped) return MAELYS_SYS_ERR_STATE;
    /* Sixteen requests unanswered at most: the keeper's queue can then
     * always hold their answers, and the answers kept here are never
     * evicted before they are read. A stop is let through all the same. */
    if (!for_stop && owned->outstanding >= OWNED_PENDING_MAX) {
        (void)flush(owned);
        (void)drain(owned, now_ms());
        if (owned->outstanding >= OWNED_PENDING_MAX) return MAELYS_SYS_ERR_WOULD_BLOCK;
    }
    if (owned->pending_count == OWNED_PENDING_MAX) {
        (void)flush(owned);
        if (owned->pending_count == OWNED_PENDING_MAX) return MAELYS_SYS_ERR_WOULD_BLOCK;
    }
    if (!for_stop && owned->next_seq >= UINT32_MAX - OWNED_STOP_SEQUENCES) return MAELYS_SYS_ERR_STATE;
    if (owned->next_seq == UINT32_MAX) return MAELYS_SYS_ERR_STATE;
    record.seq = owned->next_seq++;
    issue(owned, record.seq);
    if (enqueue(owned, record) != 0) return MAELYS_SYS_ERR_WOULD_BLOCK;
    *out_seq = record.seq;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_owned_signal(maelys_sys_owned_t *owned, int signo, int to_group, uint32_t *out_seq) {
    if (out_seq) *out_seq = 0;
    if (!owned || !out_seq || signo <= 0) return MAELYS_SYS_ERR_ARGUMENT;
    owned_record_t record = make_record(OWNED_H_SIGNAL);
    record.value = signo;
    record.a = to_group ? 1u : 0u;
    return request(owned, record, 0, out_seq);
}

maelys_sys_result_t maelys_sys_owned_stop(maelys_sys_owned_t *owned, uint64_t grace_ms, uint32_t *out_seq) {
    if (out_seq) *out_seq = 0;
    if (!owned || !out_seq) return MAELYS_SYS_ERR_ARGUMENT;
    if (owned->have_stop) {
        *out_seq = owned->stop_seq;
        return owned->final && owned->state.keeper_reaped ? MAELYS_SYS_ERR_STATE : MAELYS_SYS_OK;
    }
    owned_record_t record = make_record(OWNED_H_STOP);
    record.length = grace_ms;
    maelys_sys_result_t result = request(owned, record, 1, out_seq);
    if (result == MAELYS_SYS_OK) {
        owned->have_stop = 1;
        owned->stop_seq = *out_seq;
    }
    return result;
}

maelys_sys_result_t maelys_sys_owned_result(const maelys_sys_owned_t *owned, uint32_t seq, int *out_errno) {
    if (out_errno) *out_errno = 0;
    if (!owned || !out_errno || !seq) return MAELYS_SYS_ERR_ARGUMENT;
    for (size_t i = 0; i < ISSUED_KEPT; ++i) {
        if (owned->issued[i].seq == seq) {
            if (!owned->issued[i].answered) return MAELYS_SYS_ERR_WOULD_BLOCK;
            *out_errno = owned->issued[i].value;
            return MAELYS_SYS_OK;
        }
    }
    return MAELYS_SYS_ERR_NOT_FOUND;
}

maelys_sys_result_t maelys_sys_owned_wait(maelys_sys_owned_t *owned, uint64_t deadline_ms,
    maelys_sys_owned_state_t *out_state) {
    if (out_state) memset(out_state, 0, sizeof(*out_state));
    if (!owned || !out_state || deadline_ms == MAELYS_SYS_DEADLINE_INFINITE) return MAELYS_SYS_ERR_ARGUMENT;
    for (;;) {
        refresh(owned);
        if (owned->final && owned->state.keeper_reaped) {
            *out_state = owned->state;
            return MAELYS_SYS_OK;
        }
        uint64_t now = now_ms();
        if (now >= deadline_ms) {
            *out_state = owned->state;
            return MAELYS_SYS_ERR_TIMEOUT;
        }
        uint64_t next = deadline_ms;
        if (owned->state.next_deadline_ms < next) next = owned->state.next_deadline_ms;
        if (owned->state.keeper_reaped) {
            /* reaped but not final cannot happen; be bounded all the same */
            *out_state = owned->state;
            return MAELYS_SYS_OK;
        }
        struct pollfd fds[2];
        int count = 0;
        if (owned->ctl >= 0) {
            fds[count].fd = owned->ctl;
            fds[count].events = (short)(POLLIN | (owned->pending_count ? POLLOUT : 0));
            fds[count].revents = 0;
            ++count;
        }
        if (owned->keeper_fd >= 0) {
            fds[count].fd = owned->keeper_fd;
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
        }
        uint64_t left = next > now ? next - now : 0u;
        int ready = poll(fds, (nfds_t)count, count ? (int)(left > 60000u ? 60000u : left) : (int)(left > 10u ? 10u : left));
        if (ready < 0 && errno != EINTR) return MAELYS_SYS_ERR_OS;
    }
}

maelys_sys_result_t maelys_sys_owned_release(maelys_sys_owned_t **owned) {
    if (!owned || !*owned) return MAELYS_SYS_OK;
    maelys_sys_owned_t *handle = *owned;
    refresh(handle);
    if (!handle->state.keeper_reaped) return MAELYS_SYS_ERR_STATE;
    *owned = NULL;
    destroy(handle);
    return MAELYS_SYS_OK;
}

const char *maelys_sys_owned_keeper_path(void) {
    return MAELYS_SYS_KEEPER_PATH;
}
