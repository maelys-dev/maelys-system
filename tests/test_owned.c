/*
 * Owned process groups, black box. The programs started are this test
 * itself with --child and a mode; they are witnesses: each connects to a
 * socket of the test, says who it is (role, pid, ppid, pgid) and then
 * beats every 20 ms, so that the test knows when work stopped from the
 * production side. Their end is observed from outside, through
 * maelys_sys_process_exit_fd_open on the identity taken at their
 * registration: a witness cannot announce its own death under SIGKILL.
 *
 * The host whose death is tested is this test itself with --host, started
 * as a program of its own so that its SIGKILL leaves the test standing.
 *
 * Every expectation holds on Linux and macOS alike unless a line says
 * which host.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif

#include "maelys/sys.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s (errno %d: %s)\n", \
            __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return 1; \
    } \
} while (0)

#define CHECK_PHASE(state, wanted) do { \
    if ((state).phase != (wanted)) { \
        fprintf(stderr, "%s:%d phase %d, expected %d (leader_ended %d present %d refused %d failure %d reaped %d)\n", \
            __FILE__, __LINE__, (int)(state).phase, (int)(wanted), (state).leader_ended, \
            (state).members_present, (state).last_signal_refused, (state).keeper_failure, (state).keeper_reaped); \
        return 1; \
    } \
} while (0)

/* A fortified realpath refuses a buffer shorter than PATH_MAX. */
static char self[4096];
static char keeper[4096];
static char root[256];
static char witness_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int listener = -1;

/* The fabricated host of test_host_killed and the dormant holder of its
 * life pipe, to kill if a check fails before the test does: the keeper
 * then ends its group, and the holder, which never ends by itself, is not
 * left behind. */
static pid_t pending_host;
static pid_t pending_holder;

/* A beat: one byte of kind, then the monotonic time of its production,
 * so that the test dates the work when it was done, not when it was read. */
#define BEAT_SIZE 9u

/* A witness as the test sees it. */
typedef struct witness {
    int fd;
    char role[16];
    pid_t pid, ppid, pgid;
    maelys_sys_process_identity_t identity;
    int exit_fd;         /* readable once the process has ended, from outside */
    uint64_t last_beat;  /* when its last beat was produced, by its own clock */
    int usr1_beats;      /* beats that said SIGUSR1 was taken */
    unsigned char carry[BEAT_SIZE];
    size_t carried;
    int closed;
} witness_t;

#define WITNESSES_MAX 8

static uint64_t now_ms(void) {
    uint64_t now = 0;
    (void)maelys_sys_monotonic_ms(&now);
    return now;
}

static uint64_t after_ms(uint64_t ms) {
    uint64_t deadline = 0;
    (void)maelys_sys_deadline_after(ms, &deadline);
    return deadline;
}

static void sleep_ms(long ms) {
    struct timespec pause = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
}

static int count_open(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

/* --- what the child does, by mode ----------------------------------- */

static void say_bytes(int fd, const void *bytes, size_t length) {
    const char *text = bytes;
    while (length) {
        ssize_t written = write(fd, text, length);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return;
        text += written;
        length -= (size_t)written;
    }
}

static void say(int fd, const char *text) {
    say_bytes(fd, text, strlen(text));
}

/* Connects to the test and announces itself. -1 when the test is gone. */
static int announce(const char *path, const char *role) {
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, (socklen_t)sizeof(address)) != 0) {
        (void)close(fd);
        return -1;
    }
    char line[128];
    snprintf(line, sizeof(line), "%s %ld %ld %ld\n", role, (long)getpid(), (long)getppid(), (long)getpgrp());
    say(fd, line);
    return fd;
}

static volatile sig_atomic_t usr1_seen = 0;
static void on_usr1(int number) { (void)number; usr1_seen = 1; }

/* Beats until the test says stop, or it is gone, or 60 s have passed:
 * the witness's own bound, so that no case leaves it behind. */
static void beat_until_stopped(int fd) {
    uint64_t end = now_ms() + 60000u;
    while (now_ms() < end) {
        unsigned char beat[BEAT_SIZE];
        beat[0] = usr1_seen ? 'u' : 'b';
        usr1_seen = 0;
        uint64_t produced = now_ms();
        memcpy(beat + 1, &produced, sizeof(produced));
        ssize_t written = write(fd, beat, sizeof(beat));
        if (written < 0 && errno != EINTR && errno != EAGAIN) break;
        struct pollfd ready = {fd, POLLIN, 0};
        if (poll(&ready, 1, 20) > 0) {
            char command[8];
            ssize_t got = read(fd, command, sizeof(command));
            if (got <= 0 || memchr(command, 's', (size_t)got)) break;
        }
    }
}

static int run_child(int argc, char **argv) {
    const char *mode = argv[0];
    const char *path = argc > 1 ? argv[1] : "";
    if (strcmp(mode, "exit") == 0) return atoi(path);
    if (strcmp(mode, "fds") == 0) {
        /* what is open, written on 1 */
        char line[256];
        int n = 0;
        for (int fd = 0; fd < 64; ++fd) {
            if (fcntl(fd, F_GETFD) >= 0) n += snprintf(line + n, sizeof(line) - (size_t)n, " %d", fd);
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " | %s | cwd %s\n",
            getenv("OWNED_TEST") ? getenv("OWNED_TEST") : "-", getenv("PWD_SEEN") ? "env" : "");
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd))) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%s\n", cwd);
        }
        say(1, line);
        return 0;
    }
    if (strcmp(mode, "signals") == 0) {
        struct sigaction action;
        sigaction(SIGCHLD, NULL, &action);
        sigset_t mask;
        sigprocmask(SIG_BLOCK, NULL, &mask);
        struct sigaction term;
        sigaction(SIGTERM, NULL, &term);
        char line[128];
        snprintf(line, sizeof(line), "chld %s nocldwait %d blocked %d term %s\n",
            action.sa_handler == SIG_DFL ? "dfl" : "other", !!(action.sa_flags & SA_NOCLDWAIT),
            sigismember(&mask, SIGTERM), term.sa_handler == SIG_DFL ? "dfl" : "other");
        say(1, line);
        return 0;
    }
    /* the witnesses */
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_usr1;
    sigaction(SIGUSR1, &action, NULL);
    if (strcmp(mode, "ignore-term") == 0) signal(SIGTERM, SIG_IGN);
    if (strcmp(mode, "grand") == 0 || strcmp(mode, "grand-exit") == 0 || strcmp(mode, "escape") == 0) {
        pid_t child = fork();
        if (child == 0) {
            if (strcmp(mode, "escape") == 0) (void)setsid();
            int fd = announce(path, strcmp(mode, "escape") == 0 ? "escaped" : "grandchild");
            if (fd >= 0) beat_until_stopped(fd);
            _exit(0);
        }
        if (strcmp(mode, "grand-exit") == 0) return 0; /* the leader leaves, the grandchild stays */
    }
    int fd = announce(path, "leader");
    if (fd < 0) return 3;
    beat_until_stopped(fd);
    return 0;
}

/* --- the fabricated host -------------------------------------------- */

/* Starts the program through the keeper, says X, and waits to be killed. */
static int run_host(int argc, char **argv) {
    if (argc < 3) return 2;
    const char *keeper_path = argv[0];
    const char *mode = argv[1];
    const char *path = argv[2];
    char *program_argv[] = {self, "--child", (char *)mode, (char *)path, NULL};
    maelys_sys_process_options_t program = {.path = self, .argv = program_argv};
    maelys_sys_owned_options_t options = {.keeper_path = keeper_path, .program = &program,
        .start_deadline_ms = after_ms(30000), .heartbeat_ms = 100, .observe_ms = 2000, .linger_ms = 2000};
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_result_t result = maelys_sys_owned_spawn(&options, &owned);
    /* A dormant child that keeps a copy of every descriptor, the life
     * pipe's writer included: the keeper must see the host die by its
     * identity, not by an EOF this copy holds back. */
    pid_t holder = fork();
    if (holder == 0) {
        for (;;) pause();
    }
    char line[64];
    snprintf(line, sizeof(line), "%d %ld %ld\n", (int)result, (long)maelys_sys_owned_pid(owned), (long)holder);
    say(1, line);
    for (;;) pause();
}

/* The keeper that is not one: this test as the keeper says nothing and
 * leaves after a second. The keeper's environment is the library's, not
 * the test's, so the other fakes are shell scripts written by the test. */
static int run_fake_keeper(void) {
    sleep_ms(1000);
    return 0;
}

static char fake_exit70[sizeof(root) + 16];
static char fake_hello99[sizeof(root) + 16];

static int write_script(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
    CHECK(fd >= 0);
    say(fd, text);
    CHECK(close(fd) == 0);
    return 0;
}

/* exit70 leaves at once with a code that is no terminal state. hello99
 * writes one record on the control, 4: magic "MSOW" (57 4f 53 4d),
 * protocol 99, type 1 (HELLO), and 99 again as the protocol it names at
 * offset 20; then it waits for the end of the life pipe, 3. */
static int write_fakes(void) {
    snprintf(fake_exit70, sizeof(fake_exit70), "%s/exit70.sh", root);
    snprintf(fake_hello99, sizeof(fake_hello99), "%s/hello99.sh", root);
    if (write_script(fake_exit70, "#!/bin/sh\nexit 70\n")) return 1;
    return write_script(fake_hello99,
        "#!/bin/sh\n"
        "printf 'WOSM\\143\\000\\001\\000' >&4\n"
        "i=0; while [ $i -lt 12 ]; do printf '\\000' >&4; i=$((i + 1)); done\n"
        "printf '\\143\\000\\000\\000' >&4\n"
        "i=0; while [ $i -lt 40 ]; do printf '\\000' >&4; i=$((i + 1)); done\n"
        "cat <&3 >/dev/null\n"
        "exit 0\n");
}

/* --- the witnesses' side of the test ---------------------------------- */

static int open_listener(void) {
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    int written = snprintf(witness_path, sizeof(witness_path), "%s/w", root);
    CHECK(written > 0 && (size_t)written < sizeof(witness_path));
    memcpy(address.sun_path, witness_path, strlen(witness_path) + 1u);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    CHECK(fcntl(listener, F_SETFD, FD_CLOEXEC) == 0);
    CHECK(bind(listener, (struct sockaddr *)&address, (socklen_t)sizeof(address)) == 0);
    CHECK(listen(listener, 8) == 0);
    return 0;
}

/* Accepts witnesses and reads their beats until count have announced
 * themselves, or the deadline. */
static int gather(witness_t *witnesses, int *count, int wanted, uint64_t deadline) {
    while (*count < wanted) {
        uint64_t now = now_ms();
        if (now >= deadline) {
            fprintf(stderr, "only %d of %d witnesses announced themselves\n", *count, wanted);
            return 1;
        }
        struct pollfd ready = {listener, POLLIN, 0};
        int got = poll(&ready, 1, (int)(deadline - now));
        if (got <= 0) continue;
        int fd = accept(listener, NULL, NULL);
        CHECK(fd >= 0);
        CHECK(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
        char line[128];
        size_t length = 0;
        while (length < sizeof(line) - 1u) {
            ssize_t r = read(fd, line + length, 1);
            if (r <= 0) break;
            if (line[length] == '\n') break;
            ++length;
        }
        line[length] = '\0';
        witness_t *w = &witnesses[*count];
        memset(w, 0, sizeof(*w));
        long pid = 0, ppid = 0, pgid = 0;
        CHECK(sscanf(line, "%15s %ld %ld %ld", w->role, &pid, &ppid, &pgid) == 4);
        w->fd = fd;
        w->pid = (pid_t)pid;
        w->ppid = (pid_t)ppid;
        w->pgid = (pid_t)pgid;
        w->last_beat = now_ms();
        w->exit_fd = -1;
        CHECK(maelys_sys_process_identify(w->pid, &w->identity) == MAELYS_SYS_OK);
        CHECK(maelys_sys_process_exit_fd_open(&w->identity, &w->exit_fd) == MAELYS_SYS_OK);
        ++*count;
    }
    return 0;
}

/* Reads the beats that arrived, without waiting. */
static void take_beats(witness_t *witnesses, int count) {
    for (int i = 0; i < count; ++i) {
        witness_t *w = &witnesses[i];
        if (w->closed) continue;
        for (;;) {
            char buffer[256];
            struct pollfd ready = {w->fd, POLLIN, 0};
            if (poll(&ready, 1, 0) <= 0) break;
            ssize_t got = read(w->fd, buffer, sizeof(buffer));
            if (got <= 0) {
                w->closed = 1;
                break;
            }
            for (ssize_t j = 0; j < got; ++j) {
                w->carry[w->carried++] = (unsigned char)buffer[j];
                if (w->carried < BEAT_SIZE) continue;
                w->carried = 0;
                uint64_t produced = 0;
                memcpy(&produced, w->carry + 1, sizeof(produced));
                if (produced > w->last_beat) w->last_beat = produced;
                if (w->carry[0] == 'u') ++w->usr1_beats;
            }
        }
    }
}

/* Whether a witness has ended, seen from outside. */
static int ended(const witness_t *w) {
    struct pollfd ready = {w->exit_fd, POLLIN, 0};
    return poll(&ready, 1, 0) > 0;
}

static void drop_witnesses(witness_t *witnesses, int count) {
    for (int i = 0; i < count; ++i) {
        if (witnesses[i].fd >= 0) (void)close(witnesses[i].fd);
        if (witnesses[i].exit_fd >= 0) (void)close(witnesses[i].exit_fd);
    }
}

/* Waits, taking beats, until every witness has ended or the deadline. */
static int all_ended_by(witness_t *witnesses, int count, uint64_t deadline) {
    for (;;) {
        take_beats(witnesses, count);
        int left = 0;
        for (int i = 0; i < count; ++i) if (!ended(&witnesses[i])) ++left;
        if (!left) return 1;
        if (now_ms() >= deadline) return 0;
        sleep_ms(5);
    }
}

static int group_gone(pid_t x) {
    errno = 0;
    return kill(-x, 0) != 0 && errno == ESRCH;
}

static maelys_sys_owned_options_t options_for(maelys_sys_process_options_t *program, unsigned flags, uint64_t start_ms) {
    maelys_sys_owned_options_t options;
    memset(&options, 0, sizeof(options));
    options.keeper_path = keeper;
    options.program = program;
    options.flags = flags;
    options.start_deadline_ms = after_ms(start_ms);
    options.heartbeat_ms = 100;
    options.observe_ms = 2000;
    options.linger_ms = 2000;
    return options;
}

static int program_for(maelys_sys_process_options_t *program, char **argv, const char *mode) {
    argv[0] = self;
    argv[1] = "--child";
    argv[2] = (char *)mode;
    argv[3] = witness_path;
    argv[4] = NULL;
    memset(program, 0, sizeof(*program));
    program->path = self;
    program->argv = argv;
    return 0;
}

/* --- cases ------------------------------------------------------------- */

/* The first start: the keeper may take long to be let through on a host
 * with an on-access scanner; measured, minutes on one machine. */
static int test_stop_leader(void) {
    char *argv[5];
    maelys_sys_process_options_t program;
    program_for(&program, argv, "leader");
    maelys_sys_owned_options_t options = options_for(&program, 0, 30000);
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_OK);
    pid_t x = maelys_sys_owned_pid(owned);
    CHECK(x > 0);
    CHECK(maelys_sys_owned_fd(owned) >= 0 && maelys_sys_owned_keeper_fd(owned) >= 0);
    witness_t witnesses[WITNESSES_MAX];
    int count = 0;
    CHECK(gather(witnesses, &count, 1, after_ms(30000)) == 0);
    CHECK(witnesses[0].pid == x && witnesses[0].pgid == x);
    /* the program is the keeper's child, not the test's */
    CHECK(witnesses[0].ppid != getpid());
    CHECK(maelys_sys_owned_poll(owned, &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_RUNNING);
    /* a signal to the leader, answered */
    uint32_t seq = 0;
    CHECK(maelys_sys_owned_signal(owned, SIGUSR1, 0, &seq) == MAELYS_SYS_OK && seq);
    int result = -1;
    uint64_t deadline = after_ms(5000);
    while (maelys_sys_owned_result(owned, seq, &result) == MAELYS_SYS_ERR_WOULD_BLOCK && now_ms() < deadline) {
        (void)maelys_sys_owned_poll(owned, &state);
        sleep_ms(5);
    }
    CHECK(maelys_sys_owned_result(owned, seq, &result) == MAELYS_SYS_OK && result == 0);
    CHECK(maelys_sys_owned_result(owned, seq + 100u, &result) == MAELYS_SYS_ERR_NOT_FOUND);
    deadline = after_ms(5000);
    do {
        take_beats(witnesses, count);
        if (witnesses[0].usr1_beats) break;
        sleep_ms(5);
    } while (now_ms() < deadline);
    CHECK(witnesses[0].usr1_beats >= 1);
    /* Saturation: the keeper stopped so that no answer can come, requests
     * issued until the handle refuses, five rounds; exactly sixteen are
     * accepted each time, and once the keeper runs again every one of
     * them has its answer, none lost. The test stops the keeper by its
     * number as a test does, which a host never does. */
    pid_t keeper_pid = witnesses[0].ppid;
    for (int round = 0; round < 5; ++round) {
        CHECK(kill(keeper_pid, SIGSTOP) == 0);
        uint32_t issued[64];
        int count_issued = 0;
        maelys_sys_result_t asked = MAELYS_SYS_OK;
        for (int i = 0; i < 64 && asked == MAELYS_SYS_OK; ++i) {
            asked = maelys_sys_owned_signal(owned, SIGUSR1, 1, &issued[count_issued]);
            if (asked == MAELYS_SYS_OK) ++count_issued;
        }
        CHECK(asked == MAELYS_SYS_ERR_WOULD_BLOCK && count_issued == 16);
        CHECK(kill(keeper_pid, SIGCONT) == 0);
        deadline = after_ms(5000);
        int answered = 0;
        do {
            (void)maelys_sys_owned_poll(owned, &state);
            answered = 0;
            for (int i = 0; i < count_issued; ++i) {
                if (maelys_sys_owned_result(owned, issued[i], &result) == MAELYS_SYS_OK && result == 0) ++answered;
            }
            if (answered == count_issued) break;
            sleep_ms(2);
        } while (now_ms() < deadline);
        CHECK(answered == count_issued);
    }
    /* release refuses while the group lives */
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_ERR_STATE && owned);
    uint32_t stop = 0, again = 0;
    uint64_t asked = now_ms();
    CHECK(maelys_sys_owned_stop(owned, 3000, &stop) == MAELYS_SYS_OK);
    CHECK(maelys_sys_owned_stop(owned, 5000, &again) == MAELYS_SYS_OK && again == stop);
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_ENDED);
    /* the leader took the SIGTERM at once: the grace ended with it */
    CHECK(now_ms() - asked < 1500);
    /* the witness takes the SIGTERM of the stop */
    CHECK(state.leader_ended && state.leader.signaled && state.leader.term_signal == SIGTERM);
    CHECK(state.keeper_reaped && !state.keeper_failure && !state.members_present);
    CHECK(all_ended_by(witnesses, count, after_ms(1000)));
    CHECK(group_gone(x));
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK && !owned);
    drop_witnesses(witnesses, count);
    return 0;
}

static int test_end_with_leader(void) {
    char *argv[] = {self, "--child", "exit", "7", NULL};
    maelys_sys_process_options_t program = {.path = self, .argv = argv};
    maelys_sys_owned_options_t options = options_for(&program, MAELYS_SYS_OWNED_END_WITH_LEADER, 30000);
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    maelys_sys_result_t started = maelys_sys_owned_spawn(&options, &owned);
    /* a program this short may have ended before STARTED was read: both
     * are a success of the start */
    CHECK(started == MAELYS_SYS_OK);
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_ENDED);
    CHECK(state.leader_ended && state.leader.exited && state.leader.exit_code == 7);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    return 0;
}

/* The leader leaves, a grandchild stays: protected until stop. */
static int test_leader_exits_descendant_stays(void) {
    char *argv[5];
    maelys_sys_process_options_t program;
    program_for(&program, argv, "grand-exit");
    maelys_sys_owned_options_t options = options_for(&program, 0, 30000);
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_OK);
    pid_t x = maelys_sys_owned_pid(owned);
    witness_t witnesses[WITNESSES_MAX];
    int count = 0;
    CHECK(gather(witnesses, &count, 1, after_ms(30000)) == 0);
    CHECK(strcmp(witnesses[0].role, "grandchild") == 0 && witnesses[0].pgid == x && witnesses[0].pid != x);
    /* the leader ends, the state says so, the grandchild still beats */
    uint64_t deadline = after_ms(10000);
    do {
        CHECK(maelys_sys_owned_poll(owned, &state) == MAELYS_SYS_OK);
        if (state.leader_ended) break;
        sleep_ms(5);
    } while (now_ms() < deadline);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_LEADER_ENDED);
    CHECK(state.leader.exited && state.leader.exit_code == 0);
    uint64_t before = witnesses[0].last_beat;
    sleep_ms(100);
    take_beats(witnesses, count);
    CHECK(witnesses[0].last_beat > before);
    CHECK(!ended(&witnesses[0]));
    /* X still names the group: a signal 0 finds it */
    CHECK(!group_gone(x));
    uint32_t stop = 0;
    CHECK(maelys_sys_owned_stop(owned, 2000, &stop) == MAELYS_SYS_OK);
    uint64_t asked = now_ms();
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_ENDED);
    /* the grace ended with the leader, long gone: no two seconds spent */
    CHECK(now_ms() - asked < 1500);
    CHECK(all_ended_by(witnesses, count, after_ms(1000)));
    CHECK(group_gone(x));
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    drop_witnesses(witnesses, count);
    return 0;
}

/* SIGKILL of the host: the leader and its grandchild stop within the
 * bound, seen from outside; the group is gone. */
static int test_host_killed(void) {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    maelys_sys_process_fd_t fds[] = {{pipefd[1], 1}};
    char *argv[] = {self, "--host", keeper, "grand", witness_path, NULL};
    maelys_sys_process_options_t host_options = {.path = self, .argv = argv, .fds = fds, .fd_count = 1};
    maelys_sys_process_t *host = NULL;
    CHECK(maelys_sys_process_spawn(&host_options, &host) == MAELYS_SYS_OK);
    pending_host = maelys_sys_process_pid(host);
    CHECK(close(pipefd[1]) == 0);
    char line[64];
    size_t length = 0;
    while (length < sizeof(line) - 1u) {
        ssize_t got = read(pipefd[0], line + length, 1);
        if (got <= 0 || line[length] == '\n') break;
        ++length;
    }
    line[length] = '\0';
    CHECK(close(pipefd[0]) == 0);
    int result = -1;
    long x = 0, holder = 0;
    CHECK(sscanf(line, "%d %ld %ld", &result, &x, &holder) == 3);
    pending_holder = (pid_t)holder;
    CHECK(result == MAELYS_SYS_OK && x > 0 && holder > 0);
    witness_t witnesses[WITNESSES_MAX];
    int count = 0;
    CHECK(gather(witnesses, &count, 2, after_ms(30000)) == 0);
    for (int i = 0; i < count; ++i) CHECK(witnesses[i].pgid == (pid_t)x);
    /* the keeper is the leader's parent: its identity, to see it leave */
    pid_t keeper_pid = 0;
    for (int i = 0; i < count; ++i) if (witnesses[i].pid == (pid_t)x) keeper_pid = witnesses[i].ppid;
    CHECK(keeper_pid > 0);
    maelys_sys_process_identity_t keeper_identity;
    int keeper_exit = -1;
    CHECK(maelys_sys_process_identify(keeper_pid, &keeper_identity) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_exit_fd_open(&keeper_identity, &keeper_exit) == MAELYS_SYS_OK);
    sleep_ms(100);
    take_beats(witnesses, count);
    CHECK(maelys_sys_process_signal(host, SIGKILL) == MAELYS_SYS_OK);
    pending_host = 0;
    uint64_t killed = now_ms();
    maelys_sys_process_status_t status;
    CHECK(maelys_sys_process_wait(host, after_ms(5000), &status) == MAELYS_SYS_OK);
    CHECK(status.signaled && status.term_signal == SIGKILL);
    /* two seconds is the bound of this validation, not a promise */
    CHECK(all_ended_by(witnesses, count, killed + 2000));
    uint64_t ended_at = now_ms();
    uint64_t last_work = 0;
    for (int i = 0; i < count; ++i) {
        CHECK(witnesses[i].last_beat <= ended_at);
        if (witnesses[i].last_beat > last_work) last_work = witnesses[i].last_beat;
    }
    /* the beats are dated by their producers: no work after the bound */
    CHECK(last_work <= killed + 2000);
    fprintf(stderr, "note: owner killed, %d witnesses ended within %llu ms; last beat produced %lld ms after the kill\n",
        count, (unsigned long long)(ended_at - killed), (long long)last_work - (long long)killed);
    /* the keeper leaves once the group is empty */
    struct pollfd ready = {keeper_exit, POLLIN, 0};
    CHECK(poll(&ready, 1, 5000) > 0);
    CHECK(group_gone((pid_t)x));
    CHECK(close(keeper_exit) == 0);
    CHECK(maelys_sys_process_release(&host) == MAELYS_SYS_OK);
    /* the holder of the copy, the test's own to end */
    CHECK(kill((pid_t)holder, SIGKILL) == 0);
    pending_holder = 0;
    drop_witnesses(witnesses, count);
    return 0;
}

/* A program that ignores SIGTERM: the grace, then SIGKILL. */
static int test_ignore_term(void) {
    char *argv[5];
    maelys_sys_process_options_t program;
    program_for(&program, argv, "ignore-term");
    maelys_sys_owned_options_t options = options_for(&program, 0, 30000);
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_OK);
    witness_t witnesses[WITNESSES_MAX];
    int count = 0;
    CHECK(gather(witnesses, &count, 1, after_ms(30000)) == 0);
    uint32_t stop = 0;
    uint64_t asked = now_ms();
    CHECK(maelys_sys_owned_stop(owned, 300, &stop) == MAELYS_SYS_OK);
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_ENDED);
    CHECK(state.leader_ended && state.leader.signaled && state.leader.term_signal == SIGKILL);
    CHECK(now_ms() - asked >= 300);
    CHECK(all_ended_by(witnesses, count, after_ms(1000)));
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    drop_witnesses(witnesses, count);
    return 0;
}

/* A descendant that leaves the group by setsid is outside the frontier:
 * the group ends without it, and the test cleans it up itself. */
static int test_escape_is_outside(void) {
    char *argv[5];
    maelys_sys_process_options_t program;
    program_for(&program, argv, "escape");
    maelys_sys_owned_options_t options = options_for(&program, 0, 30000);
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_OK);
    pid_t x = maelys_sys_owned_pid(owned);
    witness_t witnesses[WITNESSES_MAX];
    int count = 0;
    CHECK(gather(witnesses, &count, 2, after_ms(30000)) == 0);
    witness_t *escaped = strcmp(witnesses[0].role, "escaped") == 0 ? &witnesses[0] : &witnesses[1];
    CHECK(escaped->pgid != x && escaped->pgid == escaped->pid);
    uint32_t stop = 0;
    CHECK(maelys_sys_owned_stop(owned, 300, &stop) == MAELYS_SYS_OK);
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_ENDED);
    sleep_ms(100);
    take_beats(witnesses, count);
    CHECK(!ended(escaped));
    CHECK(escaped->last_beat > now_ms() - 100);
    /* the test's own cleanup, by identity, not by the mechanism */
    int alive = 0;
    CHECK(maelys_sys_process_alive(&escaped->identity, &alive) == MAELYS_SYS_OK && alive);
    CHECK(kill(escaped->pid, SIGKILL) == 0);
    struct pollfd ready = {escaped->exit_fd, POLLIN, 0};
    CHECK(poll(&ready, 1, 5000) > 0);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    drop_witnesses(witnesses, count);
    return 0;
}

/* The table, the environment and the directory reach the program as
 * process_spawn hands them; the keeper's own descriptors do not. */
static int test_table_env_cwd(void) {
    int out[2];
    CHECK(pipe(out) == 0);
    maelys_sys_process_fd_t fds[] = {{out[1], 1}, {out[1], 7}};
    char *argv[] = {self, "--child", "fds", NULL};
    char *envp[] = {"OWNED_TEST=yes", NULL};
    maelys_sys_process_options_t program = {.path = self, .argv = argv, .envp = envp, .cwd = root,
        .fds = fds, .fd_count = 2};
    maelys_sys_owned_options_t options = options_for(&program, MAELYS_SYS_OWNED_END_WITH_LEADER, 30000);
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_OK);
    CHECK(close(out[1]) == 0);
    char seen[1024];
    size_t length = 0;
    for (;;) {
        ssize_t got = read(out[0], seen + length, sizeof(seen) - 1u - length);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        length += (size_t)got;
    }
    seen[length] = '\0';
    CHECK(close(out[0]) == 0);
    char expected[1024];
    snprintf(expected, sizeof(expected), " 1 7 | yes | cwd \n%s\n", root);
    if (strcmp(seen, expected) != 0) {
        /* a sanitizer runtime may add a socket of its own: say what came */
        fprintf(stderr, "%s:%d the program saw \"%s\", expected \"%s\"\n", __FILE__, __LINE__, seen, expected);
        if (!strstr(seen, " 1 7 ") || !strstr(seen, "| yes |")) return 1;
    }
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_ENDED);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    return 0;
}

/* The program starts at the default signal state even through the
 * keeper, and whatever the host blocks or sets on SIGCHLD short of
 * reaping by itself. */
static int test_signals_default(void) {
    int out[2];
    CHECK(pipe(out) == 0);
    maelys_sys_process_fd_t fds[] = {{out[1], 1}};
    char *argv[] = {self, "--child", "signals", NULL};
    maelys_sys_process_options_t program = {.path = self, .argv = argv, .fds = fds, .fd_count = 1};
    maelys_sys_owned_options_t options = options_for(&program, MAELYS_SYS_OWNED_END_WITH_LEADER, 30000);
    sigset_t block, before;
    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    CHECK(pthread_sigmask(SIG_BLOCK, &block, &before) == 0);
    struct sigaction ignore, saved;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    CHECK(sigaction(SIGTERM, &ignore, &saved) == 0);
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    maelys_sys_result_t started = maelys_sys_owned_spawn(&options, &owned);
    CHECK(sigaction(SIGTERM, &saved, NULL) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &before, NULL) == 0);
    CHECK(started == MAELYS_SYS_OK);
    CHECK(close(out[1]) == 0);
    char seen[256];
    ssize_t got = read(out[0], seen, sizeof(seen) - 1u);
    CHECK(got > 0);
    seen[got] = '\0';
    CHECK(close(out[0]) == 0);
    CHECK(strcmp(seen, "chld dfl nocldwait 0 blocked 0 term dfl\n") == 0);
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    return 0;
}

/* The keeper stopped: the host sees it unresponsive, then not. The test
 * signals the keeper by number as a test does, which a host never does. */
static int test_keeper_unresponsive(void) {
    char *argv[5];
    maelys_sys_process_options_t program;
    program_for(&program, argv, "leader");
    maelys_sys_owned_options_t options = options_for(&program, 0, 30000);
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_OK);
    witness_t witnesses[WITNESSES_MAX];
    int count = 0;
    CHECK(gather(witnesses, &count, 1, after_ms(30000)) == 0);
    pid_t keeper_pid = witnesses[0].ppid;
    CHECK(kill(keeper_pid, SIGSTOP) == 0);
    uint64_t deadline = after_ms(5000);
    do {
        CHECK(maelys_sys_owned_poll(owned, &state) == MAELYS_SYS_OK);
        if (state.keeper_unresponsive) break;
        sleep_ms(10);
    } while (now_ms() < deadline);
    CHECK(state.keeper_unresponsive);
    CHECK(state.next_deadline_ms != MAELYS_SYS_DEADLINE_INFINITE);
    CHECK(kill(keeper_pid, SIGCONT) == 0);
    deadline = after_ms(5000);
    do {
        CHECK(maelys_sys_owned_poll(owned, &state) == MAELYS_SYS_OK);
        if (!state.keeper_unresponsive) break;
        sleep_ms(10);
    } while (now_ms() < deadline);
    CHECK(!state.keeper_unresponsive);
    uint32_t stop = 0;
    CHECK(maelys_sys_owned_stop(owned, 300, &stop) == MAELYS_SYS_OK);
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_ENDED);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    drop_witnesses(witnesses, count);
    return 0;
}

static int test_refusals(void) {
    char *argv[] = {self, "--child", "exit", "0", NULL};
    maelys_sys_process_options_t program = {.path = self, .argv = argv};
    maelys_sys_owned_options_t options = options_for(&program, 0, 30000);
    maelys_sys_owned_t *owned = (maelys_sys_owned_t *)1;
    CHECK(maelys_sys_owned_spawn(NULL, &owned) == MAELYS_SYS_ERR_ARGUMENT && !owned);
    CHECK(maelys_sys_owned_spawn(&options, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    options.keeper_path = "relative/keeper";
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_ARGUMENT);
    options.keeper_path = keeper;
    program.flags = MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_ARGUMENT);
    program.flags = 0;
    options.flags = 1u << 7;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_ARGUMENT);
    options.flags = 0;
    maelys_sys_process_fd_t many[MAELYS_SYS_OWNED_MAX_FDS + 1];
    for (size_t i = 0; i <= MAELYS_SYS_OWNED_MAX_FDS; ++i) {
        many[i].source = 0;
        many[i].target = (int)i + 3;
    }
    program.fds = many;
    program.fd_count = MAELYS_SYS_OWNED_MAX_FDS + 1;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_ARGUMENT);
    program.fds = NULL;
    program.fd_count = 0;
    /* SIGCHLD reaped by the kernel: refused before anything exists */
    struct sigaction auto_reap, saved;
    memset(&auto_reap, 0, sizeof(auto_reap));
    auto_reap.sa_handler = SIG_DFL;
    auto_reap.sa_flags = SA_NOCLDWAIT;
    CHECK(sigaction(SIGCHLD, &auto_reap, &saved) == 0);
    maelys_sys_result_t refused = maelys_sys_owned_spawn(&options, &owned);
    CHECK(sigaction(SIGCHLD, &saved, NULL) == 0);
    CHECK(refused == MAELYS_SYS_ERR_STATE && !owned);
    /* no keeper there: nothing started in its place */
    char missing[sizeof(root) + 16];
    snprintf(missing, sizeof(missing), "%s/no-keeper", root);
    options.keeper_path = missing;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_UNSUPPORTED && !owned);
    options.keeper_path = keeper;
    /* no program: the start's own error, no handle, the keeper gone */
    program.path = missing;
    int before = count_open();
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_NOT_FOUND && !owned);
    CHECK(count_open() == before);
    program.path = root; /* a directory: not runnable */
    maelys_sys_result_t denied = maelys_sys_owned_spawn(&options, &owned);
    CHECK(denied == MAELYS_SYS_ERR_OS && !owned && errno == EACCES);
    CHECK(count_open() == before);
    return 0;
}

/* A keeper that is not one: a timeout leaves a handle to follow; an
 * early exit is a loss. The test is its own fake keeper. */
static int test_fake_keeper(void) {
    char *argv[] = {self, "--child", "exit", "0", NULL};
    maelys_sys_process_options_t program = {.path = self, .argv = argv};
    maelys_sys_owned_options_t options = options_for(&program, 0, 600);
    options.keeper_path = self; /* says HELLO never, leaves after 1 s */
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_TIMEOUT);
    CHECK(owned != NULL);
    CHECK(maelys_sys_owned_poll(owned, &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_START_FAILED);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_ERR_STATE && owned);
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK(state.keeper_reaped);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK && !owned);
    /* leaves at once with a code that is no terminal state: gone before
     * any program could exist, nothing is left to follow */
    options.keeper_path = fake_exit70;
    options.start_deadline_ms = after_ms(10000);
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_CLOSED && !owned);
    /* speaks another protocol: incompatible, not closed, nothing started */
    options.keeper_path = fake_hello99;
    options.start_deadline_ms = after_ms(10000);
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_UNSUPPORTED && !owned);
    return 0;
}

#if defined(__linux__)
/* The CPU time of a process in clock ticks, from /proc. */
static long cpu_ticks(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    char line[1024];
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    const char *cursor = strrchr(line, ')');
    if (!cursor) return -1;
    long utime = 0, stime = 0;
    /* fields 3 to 15 after the name: state ... utime stime */
    if (sscanf(cursor + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &utime, &stime) != 2) return -1;
    return utime + stime;
}

/* A group that cannot be seen empty: the test makes itself the adopter of
 * the leader's orphans and does not reap the grandchild, whose zombie
 * keeps X occupied on Linux. The keeper reaches UNCERTAIN, stays idle
 * while it waits, and ends the group once the zombie is reaped. */
static int test_uncertain_then_ended(void) {
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    char *argv[5];
    maelys_sys_process_options_t program;
    program_for(&program, argv, "grand");
    maelys_sys_owned_options_t options = options_for(&program, 0, 30000);
    options.observe_ms = 300;
    options.linger_ms = 10000;
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_OK);
    pid_t x = maelys_sys_owned_pid(owned);
    witness_t witnesses[WITNESSES_MAX];
    int count = 0;
    CHECK(gather(witnesses, &count, 2, after_ms(30000)) == 0);
    pid_t keeper_pid = 0, grandchild = 0;
    for (int i = 0; i < count; ++i) {
        if (witnesses[i].pid == x) keeper_pid = witnesses[i].ppid;
        else grandchild = witnesses[i].pid;
    }
    CHECK(keeper_pid > 0 && grandchild > 0);
    uint32_t stop = 0;
    CHECK(maelys_sys_owned_stop(owned, 100, &stop) == MAELYS_SYS_OK);
    uint64_t deadline = after_ms(10000);
    do {
        CHECK(maelys_sys_owned_poll(owned, &state) == MAELYS_SYS_OK);
        if (state.phase == MAELYS_SYS_OWNED_UNCERTAIN) break;
        sleep_ms(10);
    } while (now_ms() < deadline);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_UNCERTAIN);
    CHECK(state.members_present);
    CHECK(all_ended_by(witnesses, count, after_ms(1000)));
    /* idle while uncertain: a second of waiting costs the keeper little */
    long ticks_before = cpu_ticks(keeper_pid);
    CHECK(ticks_before >= 0);
    sleep_ms(1000);
    long ticks_after = cpu_ticks(keeper_pid);
    CHECK(ticks_after >= ticks_before);
    fprintf(stderr, "note: the keeper spent %ld ticks of CPU in a second of UNCERTAIN\n", ticks_after - ticks_before);
    CHECK(ticks_after - ticks_before < 20);
    /* the zombie reaped by its adopter, the group is empty, the keeper sees it */
    CHECK(waitpid(grandchild, NULL, 0) == grandchild);
    CHECK(maelys_sys_owned_wait(owned, after_ms(10000), &state) == MAELYS_SYS_OK);
    CHECK_PHASE(state, MAELYS_SYS_OWNED_ENDED);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    drop_witnesses(witnesses, count);
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 0) == 0);
    /* whatever else was adopted meanwhile is reaped now */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    return 0;
}
#else
static int test_uncertain_then_ended(void) {
    fprintf(stderr, "note: the uncertain group needs a subreaper: Linux only, skipped here\n");
    return 0;
}
#endif

/* What a handle and the processes around it look like, for a failure that
 * is not understood yet: the state, then the test's children and theirs. */
static void say_where(const char *what, maelys_sys_owned_t *owned) {
    maelys_sys_owned_state_t state;
    (void)maelys_sys_owned_poll(owned, &state);
    fprintf(stderr, "%s: phase %d leader_ended %d (exited %d code %d signaled %d sig %d) unreaped %d refused %d "
        "present %d unresponsive %d failure %d reaped %d elsewhere %d truncated %d after_timeout %d "
        "from_exit %d want_write %d x %ld\n", what, (int)state.phase, state.leader_ended,
        state.leader.exited, state.leader.exit_code, state.leader.signaled, state.leader.term_signal,
        state.leader_unreaped, state.last_signal_refused, state.members_present, state.keeper_unresponsive,
        state.keeper_failure, state.keeper_reaped, state.keeper_reaped_elsewhere, state.truncated_final,
        state.started_after_timeout, state.state_from_exit_code, state.want_write,
        (long)maelys_sys_owned_pid(owned));
    char command[200];
    snprintf(command, sizeof(command),
        "ps -axo pid,ppid,pgid,stat,command | awk '$2 == %ld || $3 == %ld' >&2", (long)getpid(),
        (long)maelys_sys_owned_pid(owned));
    int listed = system(command);
    (void)listed;
#if defined(__APPLE__)
    /* where the program is, if it still runs */
    if (maelys_sys_owned_pid(owned) > 0) {
        snprintf(command, sizeof(command), "sample %ld 1 2>/dev/null | sed -n '/Call graph/,/Total number/p' | head -40 >&2",
            (long)maelys_sys_owned_pid(owned));
        listed = system(command);
        (void)listed;
    }
#endif
}

/* Starts that succeed and starts that fail, by turns, many times: nothing
 * left open, nobody left behind. A hundred cycles; MAELYS_SYS_OWNED_CYCLES
 * sets another number, a thousand for the campaign. */
static int test_many_cycles(void) {
    int before = count_open();
    long cycles = 100;
    const char *asked = getenv("MAELYS_SYS_OWNED_CYCLES");
    if (asked && atol(asked) > 0) cycles = atol(asked);
    char missing[sizeof(root) + 16];
    snprintf(missing, sizeof(missing), "%s/no-program", root);
    for (long cycle = 0; cycle < cycles; ++cycle) {
        char *argv[] = {self, "--child", "exit", "0", NULL};
        maelys_sys_process_options_t program = {.path = cycle % 2 ? missing : self, .argv = argv};
        maelys_sys_owned_options_t options = options_for(&program, MAELYS_SYS_OWNED_END_WITH_LEADER, 30000);
        maelys_sys_owned_t *owned = NULL;
        maelys_sys_owned_state_t state;
        if (cycle % 2) {
            CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_ERR_NOT_FOUND && !owned);
            continue;
        }
        CHECK(maelys_sys_owned_spawn(&options, &owned) == MAELYS_SYS_OK);
        /* Thirty seconds: an exec was measured staying in _dyld_start for
         * over ten on one machine, the program then not started at all. */
        if (maelys_sys_owned_wait(owned, after_ms(30000), &state) != MAELYS_SYS_OK) {
            fprintf(stderr, "cycle %ld: no final state in 30 s\n", cycle);
            say_where("at the deadline", owned);
            return 1;
        }
        CHECK_PHASE(state, MAELYS_SYS_OWNED_ENDED);
        CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    }
    CHECK(count_open() == before);
    errno = 0;
    pid_t left = waitpid(-1, NULL, WNOHANG);
    if (left >= 0 || errno != ECHILD) {
        /* Who is still there, and whether it is a delay or a leak: the
         * children listed, then one second later, with what a reaping
         * finds by then. */
        char command[160];
        snprintf(command, sizeof(command), "ps -axo pid,ppid,pgid,stat,command | awk '$2 == %ld' >&2", (long)getpid());
        fprintf(stderr, "children left at the end of the cycles (pid ppid pgid stat command):\n");
        /* GCC does not take a (void) cast for warn_unused_result */
        int listed = system(command);
        (void)listed;
        sleep_ms(1000);
        int status = 0;
        errno = 0;
        pid_t later = waitpid(-1, &status, WNOHANG);
        fprintf(stderr, "one second later: waitpid(-1) = %ld (errno %d), %s\n", (long)later, errno,
            later > 0 ? (WIFEXITED(status) ? "exited" : "signaled") : later == 0 ? "still alive" : "none");
        listed = system(command);
        (void)listed;
    }
    CHECK(left < 0 && errno == ECHILD);
    return 0;
}

static int resolve_self(const char *argv0) {
    if (realpath(argv0, self)) return 0;
#if defined(__linux__)
    ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length <= 0) return 1;
    self[length] = '\0';
    return 0;
#else
    return 1;
#endif
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--child") == 0) return run_child(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "--start-timeout-ms") == 0 && getenv("MAELYS_SYS_KEEPER")) {
        return run_fake_keeper();
    }
    if (resolve_self(argv[0]) != 0) return 1;
    if (argc >= 2 && strcmp(argv[1], "--host") == 0) return run_host(argc - 2, argv + 2);
    /* the keeper built beside this test: tests/ and libexec/ of one build */
    const char *from_env = getenv("MAELYS_SYS_KEEPER_UNDER_TEST");
    if (from_env) {
        snprintf(keeper, sizeof(keeper), "%s", from_env);
    } else {
        char *slash = strrchr(self, '/');
        if (!slash) return 1;
        snprintf(keeper, sizeof(keeper), "%.*s/../libexec/maelys-sys/keeper", (int)(slash - self), self);
    }
    char resolved[4096];
    if (realpath(keeper, resolved)) snprintf(keeper, sizeof(keeper), "%s", resolved);
    const char *base = getenv("TMPDIR");
    if (!base || base[0] != '/') base = "/tmp";
    int written = snprintf(root, sizeof(root), "%s/maelys-sys-owned.XXXXXX", base);
    if (written <= 0 || (size_t)written >= sizeof(root) || !mkdtemp(root)) return 1;
    /* as the program will see it: /var is a link on macOS */
    if (realpath(root, resolved) && strlen(resolved) < sizeof(root)) snprintf(root, sizeof(root), "%s", resolved);
    if (open_listener() != 0) return 1;
    if (write_fakes() != 0) return 1;
    int before = count_open();
    static int (*const cases[])(void) = {test_stop_leader, test_end_with_leader, test_leader_exits_descendant_stays,
        test_host_killed, test_ignore_term, test_escape_is_outside, test_table_env_cwd, test_signals_default,
        test_keeper_unresponsive, test_refusals, test_fake_keeper, test_uncertain_then_ended, test_many_cycles};
    static const char *const names[] = {"stop_leader", "end_with_leader", "leader_exits_descendant_stays",
        "host_killed", "ignore_term", "escape_is_outside", "table_env_cwd", "signals_default",
        "keeper_unresponsive", "refusals", "fake_keeper", "uncertain_then_ended", "many_cycles"};
    int failed = 0;
    /* MAELYS_SYS_TEST_TIMING set: how long each case took, on stderr */
    const char *timing = getenv("MAELYS_SYS_TEST_TIMING");
    for (size_t i = 0; !failed && i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint64_t started = now_ms();
        failed = cases[i]();
        if (timing) fprintf(stderr, "timing: %s %llu ms\n", names[i], (unsigned long long)(now_ms() - started));
    }
    if (failed) {
        if (pending_host > 0) (void)kill(pending_host, SIGKILL);
        if (pending_holder > 0) (void)kill(pending_holder, SIGKILL);
        return 1;
    }
    if (count_open() != before) {
        fprintf(stderr, "descriptors left open\n");
        return 1;
    }
    (void)close(listener);
    (void)unlink(witness_path);
    (void)unlink(fake_exit70);
    (void)unlink(fake_hello99);
    if (rmdir(root) != 0) {
        fprintf(stderr, "work directory not empty: %s\n", root);
        return 1;
    }
    puts("ok - owned a leader stopped: a signal taken and answered, saturation refused at sixteen and every accepted request answered, release refused while alive, group gone");
    puts("ok - owned END_WITH_LEADER: a program that exits ends its group");
    puts("ok - owned the leader leaves, a grandchild stays protected until stop; the grace ends with the leader");
    puts("ok - owned the host killed by SIGKILL, a copy of its life pipe held elsewhere: leader and grandchild stopped within the bound, seen from outside");
    puts("ok - owned a program that ignores SIGTERM: the grace, then SIGKILL");
    puts("ok - owned a descendant that leaves by setsid is outside the frontier");
    puts("ok - owned table, environment and directory reach the program; the keeper's descriptors do not");
    puts("ok - owned the program starts at the default signal state through the keeper");
    puts("ok - owned a stopped keeper is seen unresponsive, then not");
    puts("ok - owned refusals before anything exists; a missing keeper or program starts nothing");
    puts("ok - owned a keeper that is not one: a timeout leaves a handle to follow, an early exit is a loss, another protocol is unsupported");
    puts("ok - owned a group that cannot be seen empty is UNCERTAIN, idle, then ENDED once its zombie is reaped (Linux; skipped elsewhere)");
    puts("ok - owned a hundred cycles of starts and refused starts: nothing left open, nobody left behind");
    return 0;
}
