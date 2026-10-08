/*
 * Processes, black box. The programs started are this test itself, with
 * --child and a mode: each mode is a few lines that report what the child
 * sees. Every expectation holds on Linux and macOS alike unless a line
 * says which host.
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
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s (errno %d: %s)\n", \
            __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return 1; \
    } \
} while (0)

/* A descriptor the program connected itself, to a named peer, is not one
 * the table let through: a sanitizer runtime on macOS 15 opens one to the
 * log daemon before main. Such entries are set apart and noted. */
static void set_apart_named_sockets(char *seen) {
    char *entry;
    while ((entry = strstr(seen, "(sock:")) != NULL) {
        char *start = entry;
        while (start > seen && start[-1] != ' ') --start;
        char *end = strchr(entry, ')');
        if (!end) break;
        ++end;
        fprintf(stderr, "note: the program opened %.*s itself; set apart\n", (int)(end - start), start);
        if (start > seen) --start; /* the space before */
        memmove(start, end, strlen(end) + 1);
    }
}

/* What the child saw, when it is not what was expected. */
#define CHECK_SEEN(seen, expected) do { \
    set_apart_named_sockets(seen); \
    if (strcmp((seen), (expected)) != 0) { \
        fprintf(stderr, "%s:%d the child saw \"%s\", expected \"%s\"\n", \
            __FILE__, __LINE__, (seen), (expected)); \
        return 1; \
    } \
} while (0)

#define PATH_ROOM 512

/* A fortified realpath refuses a buffer shorter than PATH_MAX. */
static char self[4096];
static char root[256];

static int count_open(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

static void sleep_ms(long ms) {
    struct timespec pause = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
}

/* Waits until the program has ended, without reaping it: a sanitized
 * host starts a program slowly, and a fixed pause would race it. */
static int ended_unreaped(pid_t pid) {
    siginfo_t info;
    while (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOWAIT) != 0) {
        if (errno != EINTR) return 0;
    }
    return 1;
}

static uint64_t now_ms(void) {
    uint64_t now = 0;
    (void)maelys_sys_monotonic_ms(&now);
    return now;
}

/* --- what the child does, by mode ----------------------------------- */

static void say(const char *text) {
    size_t length = strlen(text);
    while (length) {
        ssize_t written = write(1, text, length);
        if (written <= 0) return;
        text += written;
        length -= (size_t)written;
    }
}


/* What a descriptor is, for the listing: a sanitizer runtime may open one
 * of its own in the program, and the listing then says which it is. */
static const char *fd_kind(int fd) {
    struct stat status;
    if (fstat(fd, &status) != 0) return "?";
    if (S_ISSOCK(status.st_mode)) {
        /* A socket with a named peer was connected by the program itself,
         * a runtime's log socket for one: no socket of the table has a
         * name. Said as such, so that the parent can set it apart. */
        static char named[128];
        struct sockaddr_un peer;
        socklen_t length = (socklen_t)sizeof(peer);
        memset(&peer, 0, sizeof(peer));
        if (getpeername(fd, (struct sockaddr *)&peer, &length) == 0 &&
            peer.sun_family == AF_UNIX && length > (socklen_t)offsetof(struct sockaddr_un, sun_path) &&
            peer.sun_path[0]) {
            snprintf(named, sizeof(named), "sock:%.100s", peer.sun_path);
            return named;
        }
        return "sock";
    }
    if (S_ISFIFO(status.st_mode)) return "fifo";
    if (S_ISCHR(status.st_mode)) return "chr";
    if (S_ISREG(status.st_mode)) return "reg";
    if (S_ISDIR(status.st_mode)) return "dir";
    return "other";
}

/* fdlist [FD...]: the open descriptors, with their kind from 3 up, what 0
 * says, and one byte from each descriptor named. */
static int child_fdlist(int argc, char **argv) {
    char line[512];
    int n = snprintf(line, sizeof(line), "fds:");
    for (int fd = 0; fd < 64; ++fd) {
        if (fcntl(fd, F_GETFD) < 0) continue;
        if (fd < 3) n += snprintf(line + n, sizeof(line) - (size_t)n, " %d", fd);
        else n += snprintf(line + n, sizeof(line) - (size_t)n, " %d(%s)", fd, fd_kind(fd));
    }
    char byte = 0;
    ssize_t got = read(0, &byte, 1);
    n += snprintf(line + n, sizeof(line) - (size_t)n, " fd0:%s",
        got == 0 ? "eof" : got > 0 ? "byte" : "closed");
    for (int index = 0; index < argc; ++index) {
        /* One byte from a descriptor of the table, without waiting: a
         * descriptor that is not the table's could have nothing to say. */
        int fd = atoi(argv[index]);
        byte = '.';
        if (fcntl(fd, F_GETFD) >= 0) {
            int flags = fcntl(fd, F_GETFL);
            if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            got = read(fd, &byte, 1);
            if (got == 0) byte = '-';
            else if (got < 0) byte = '~';
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, " fd%d:%c", fd, byte);
    }
    say(line);
    return 0;
}

static int child_env(void) {
    extern char **environ;
    int count = 0;
    for (char **entry = environ; *entry; ++entry) ++count;
    char line[64];
    snprintf(line, sizeof(line), "env:%d var:%s", count, getenv("MAELYS_SYS_TEST_VAR") ? "yes" : "no");
    say(line);
    return 0;
}

static int child_where(void) {
    char cwd[PATH_ROOM], line[PATH_ROOM + 64];
    if (!getcwd(cwd, sizeof(cwd))) return 3;
    snprintf(line, sizeof(line), "cwd:%s sid:%d pgid:%d", cwd,
        getsid(0) == getpid(), getpgid(0) == getpid());
    say(line);
    return 0;
}

static int run_child(int argc, char **argv) {
    const char *mode = argv[0];
    if (strcmp(mode, "fdlist") == 0) return child_fdlist(argc - 1, argv + 1);
    if (strcmp(mode, "sleep") == 0) {
        sleep_ms(atol(argv[1]));
        return 0;
    }
    if (strcmp(mode, "ignore-term") == 0) {
        signal(SIGTERM, SIG_IGN);
        say("ready");
        sleep_ms(10000);
        return 0;
    }
    if (strcmp(mode, "exit") == 0) return atoi(argv[1]);
    if (strcmp(mode, "env") == 0) return child_env();
    if (strcmp(mode, "where") == 0) return child_where();
    return 99;
}

/* --- the parent's side ---------------------------------------------- */

typedef struct launch {
    const char *mode;
    const char *arguments[4];
    char *const *envp;
    const char *cwd;
    const maelys_sys_process_fd_t *fds;
    size_t fd_count;
    unsigned flags;
} launch_t;

static maelys_sys_result_t start(const launch_t *launch, maelys_sys_process_t **out) {
    char *argv[8] = {self, "--child", (char *)launch->mode, NULL};
    size_t argc = 3;
    for (size_t index = 0; index < 4 && launch->arguments[index]; ++index) {
        argv[argc++] = (char *)launch->arguments[index];
    }
    argv[argc] = NULL;
    maelys_sys_process_options_t options = {
        .path = self, .argv = argv, .envp = launch->envp, .cwd = launch->cwd,
        .fds = launch->fds, .fd_count = launch->fd_count, .flags = launch->flags
    };
    return maelys_sys_process_spawn(&options, out);
}

/* Reads fd until end of stream, within 10 s: a sanitized program on a
 * loaded host takes seconds to start. */
static int read_all(int fd, char *buffer, size_t capacity) {
    size_t length = 0;
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(10000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_set_nonblocking(fd) == MAELYS_SYS_OK);
    for (;;) {
        unsigned flags = 0;
        CHECK(maelys_sys_fd_wait(fd, MAELYS_SYS_INTEREST_READ, deadline, &flags) == MAELYS_SYS_OK);
        ssize_t got = read(fd, buffer + length, capacity - 1 - length);
        if (got < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        CHECK(got >= 0);
        if (got == 0) break;
        length += (size_t)got;
        CHECK(length < capacity - 1);
    }
    buffer[length] = '\0';
    return 0;
}

static int finish(maelys_sys_process_t **process, int expected_code) {
    maelys_sys_process_status_t status;
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(10000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_wait(*process, deadline, &status) == MAELYS_SYS_OK);
    CHECK(status.exited && status.exit_code == expected_code && !status.signaled);
    CHECK(maelys_sys_process_release(process) == MAELYS_SYS_OK && *process == NULL);
    return 0;
}

/* The STDIO layout: a socket pair on 0 and 1, 2 inherited. */
static int test_stdio_layout(void) {
    int pair[2];
    CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, pair) == MAELYS_SYS_OK);
    CHECK(shutdown(pair[0], SHUT_WR) == 0);
    maelys_sys_process_fd_t table[3] = {{pair[1], 0}, {pair[1], 1}, {2, 2}};
    launch_t launch = {.mode = "fdlist", .fds = table, .fd_count = 3};
    maelys_sys_process_t *process = NULL;
    CHECK(start(&launch, &process) == MAELYS_SYS_OK && process);
    CHECK(maelys_sys_fd_close(&pair[1]) == MAELYS_SYS_OK);
    char seen[512];
    CHECK(read_all(pair[0], seen, sizeof(seen)) == 0);
    CHECK_SEEN(seen, "fds: 0 1 2 fd0:eof");
    CHECK(finish(&process, 0) == 0);
    CHECK(maelys_sys_fd_close(&pair[0]) == MAELYS_SYS_OK);
    return 0;
}

/* The ISOLATED layout, with forty stray descriptors open in the parent:
 * /dev/null on 0, one pipe on 1 and 2, a pair on 3, a profile on 4. */
static int test_isolated_layout(void) {
    int stray[40];
    for (int index = 0; index < 40; ++index) {
        stray[index] = open("/dev/null", O_RDONLY);
        CHECK(stray[index] >= 0);
    }
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int out[2], pair[2];
    CHECK(devnull >= 0);
    CHECK(maelys_sys_pipe_cloexec(out) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, pair) == MAELYS_SYS_OK);
    int profile = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(profile >= 0);
    maelys_sys_process_fd_t table[5] = {
        {devnull, 0}, {out[1], 2}, {out[1], 1}, {pair[1], 3}, {profile, 4}
    };
    /* The listing alone says what is open; a probe of a number outside the
     * table could land on a descriptor the program opened itself. */
    launch_t launch = {.mode = "fdlist", .arguments = {"3", "4"}, .fds = table, .fd_count = 5};
    maelys_sys_process_t *process = NULL;
    CHECK(start(&launch, &process) == MAELYS_SYS_OK && process);
    CHECK(maelys_sys_fd_close(&out[1]) == MAELYS_SYS_OK);
    CHECK(shutdown(pair[0], SHUT_WR) == 0);
    char seen[512];
    CHECK(read_all(out[0], seen, sizeof(seen)) == 0);
    CHECK_SEEN(seen, "fds: 0 1 2 3(sock) 4(chr) fd0:eof fd3:- fd4:-");
    CHECK(finish(&process, 0) == 0);
    for (int index = 0; index < 40; ++index) CHECK(maelys_sys_fd_close(&stray[index]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&devnull) == MAELYS_SYS_OK && maelys_sys_fd_close(&profile) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&out[0]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&pair[0]) == MAELYS_SYS_OK && maelys_sys_fd_close(&pair[1]) == MAELYS_SYS_OK);
    return 0;
}

/* The table as a whole: an exchange of two numbers, and one source on two
 * targets. Each pipe carries a letter the child reads back. 0, 1 and 2 are
 * all named: a sanitizer runtime opens /dev/null on a standard descriptor
 * it finds closed, and the layouts above already prove what a target not
 * named becomes. */
static int test_table_as_a_whole(void) {
    int a[2], b[2], out[2];
    CHECK(maelys_sys_pipe_cloexec(a) == MAELYS_SYS_OK && maelys_sys_pipe_cloexec(b) == MAELYS_SYS_OK);
    CHECK(maelys_sys_pipe_cloexec(out) == MAELYS_SYS_OK);
    /* Three letters in a: the exchange reads one, the two targets below one each. */
    CHECK(write(a[1], "AAA", 3) == 3 && write(b[1], "B", 1) == 1);
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(devnull >= 0);
    /* {s1->s2, s2->s1}: the child reads B where the parent had A. */
    int s1 = a[0], s2 = b[0];
    char s1_text[16], s2_text[16];
    snprintf(s1_text, sizeof(s1_text), "%d", s1);
    snprintf(s2_text, sizeof(s2_text), "%d", s2);
    maelys_sys_process_fd_t exchange[5] = {{devnull, 0}, {out[1], 1}, {devnull, 2}, {s1, s2}, {s2, s1}};
    launch_t launch = {.mode = "fdlist", .arguments = {s1_text, s2_text}, .fds = exchange, .fd_count = 5};
    maelys_sys_process_t *process = NULL;
    CHECK(start(&launch, &process) == MAELYS_SYS_OK && process);
    char seen[512], expected[512];
    int out_reader = out[0];
    CHECK(maelys_sys_fd_close(&out[1]) == MAELYS_SYS_OK);
    CHECK(read_all(out_reader, seen, sizeof(seen)) == 0);
    snprintf(expected, sizeof(expected), "fds: 0 1 2 %d(fifo) %d(fifo) fd0:eof fd%d:B fd%d:A",
        s1 < s2 ? s1 : s2, s1 < s2 ? s2 : s1, s1, s2);
    CHECK_SEEN(seen, expected);
    CHECK(finish(&process, 0) == 0);
    CHECK(maelys_sys_fd_close(&out_reader) == MAELYS_SYS_OK);
    /* {a->6, a->7}: both read the pipe, one letter each. */
    CHECK(maelys_sys_pipe_cloexec(out) == MAELYS_SYS_OK);
    maelys_sys_process_fd_t twice[5] = {{devnull, 0}, {out[1], 1}, {devnull, 2}, {a[0], 6}, {a[0], 7}};
    launch_t again = {.mode = "fdlist", .arguments = {"6", "7"}, .fds = twice, .fd_count = 5};
    CHECK(start(&again, &process) == MAELYS_SYS_OK && process);
    CHECK(maelys_sys_fd_close(&out[1]) == MAELYS_SYS_OK);
    CHECK(read_all(out[0], seen, sizeof(seen)) == 0);
    CHECK_SEEN(seen, "fds: 0 1 2 6(fifo) 7(fifo) fd0:eof fd6:A fd7:A");
    CHECK(finish(&process, 0) == 0);
    CHECK(maelys_sys_fd_close(&out[0]) == MAELYS_SYS_OK && maelys_sys_fd_close(&devnull) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&a[0]) == MAELYS_SYS_OK && maelys_sys_fd_close(&a[1]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&b[0]) == MAELYS_SYS_OK && maelys_sys_fd_close(&b[1]) == MAELYS_SYS_OK);
    return 0;
}

/* No process exists after a refused start. On macOS the process a
 * refused posix_spawn aborted stays visible to a wait for a few tens of
 * microseconds, measured, and leaves no zombie: looked for again. */
static int no_child_left(void) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        errno = 0;
        pid_t got = waitpid(-1, NULL, WNOHANG);
        if (got == -1 && errno == ECHILD) return 1;
        if (got != 0) return 0;
        sleep_ms(1);
    }
    return 0;
}

static int test_refusals(void) {
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(devnull >= 0);
    maelys_sys_process_t *process = (maelys_sys_process_t *)&root;
    char *argv[] = {self, "--child", "exit", "0", NULL};
    maelys_sys_process_options_t options = {.path = self, .argv = argv};
    maelys_sys_process_fd_t twice[2] = {{devnull, 3}, {devnull, 3}};
    options.fds = twice;
    options.fd_count = 2;
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_ERR_ARGUMENT && !process);
    maelys_sys_process_fd_t negative[1] = {{devnull, -1}};
    options.fds = negative;
    options.fd_count = 1;
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_ERR_ARGUMENT);
    int closed = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(closed >= 0 && maelys_sys_fd_close(&closed) == MAELYS_SYS_OK);
    maelys_sys_process_fd_t gone[1] = {{closed < 0 ? 1000 : closed, 3}};
    options.fds = gone;
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_ERR_ARGUMENT);
    maelys_sys_process_fd_t many[MAELYS_SYS_PROCESS_MAX_FDS + 1];
    for (unsigned index = 0; index <= MAELYS_SYS_PROCESS_MAX_FDS; ++index) {
        many[index].source = devnull;
        many[index].target = (int)index;
    }
    options.fds = many;
    options.fd_count = MAELYS_SYS_PROCESS_MAX_FDS + 1;
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_ERR_ARGUMENT);
    options.fds = NULL;
    options.fd_count = 0;
    options.flags = MAELYS_SYS_PROCESS_NEW_SESSION | MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP;
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_ERR_ARGUMENT);
    options.flags = 0;
    options.path = "tests/test_process";
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_ERR_ARGUMENT);
    options.path = self;
    options.argv = NULL;
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_process_spawn(NULL, &process) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_process_spawn(&options, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(no_child_left());
    CHECK(maelys_sys_fd_close(&devnull) == MAELYS_SYS_OK);
    return 0;
}

/* A path that names nothing, and a file the caller may not run. */
static int test_start_refused(void) {
    int before = count_open();
    maelys_sys_process_t *process = (maelys_sys_process_t *)&root;
    char *argv[] = {"x", NULL};
    char missing[PATH_ROOM], plain[PATH_ROOM];
    snprintf(missing, sizeof(missing), "%s/missing", root);
    snprintf(plain, sizeof(plain), "%s/plain", root);
    maelys_sys_process_options_t options = {.path = missing, .argv = argv};
    errno = 0;
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(errno == ENOENT && !process && no_child_left());
    int fd = open(plain, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    options.path = plain;
    errno = 0;
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EACCES && !process && no_child_left());
    CHECK(count_open() == before);
    CHECK(unlink(plain) == 0);
    return 0;
}

static int test_environment(void) {
    int out[2];
    char seen[512];
    maelys_sys_process_t *process = NULL;
    CHECK(maelys_sys_pipe_cloexec(out) == MAELYS_SYS_OK);
    maelys_sys_process_fd_t table[1] = {{out[1], 1}};
    char *envp[] = {"MAELYS_SYS_TEST_VAR=1", "OTHER=2", NULL};
    launch_t explicit = {.mode = "env", .envp = envp, .fds = table, .fd_count = 1};
    CHECK(start(&explicit, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&out[1]) == MAELYS_SYS_OK);
    CHECK(read_all(out[0], seen, sizeof(seen)) == 0);
    CHECK_SEEN(seen, "env:2 var:yes");
    CHECK(finish(&process, 0) == 0);
    CHECK(maelys_sys_fd_close(&out[0]) == MAELYS_SYS_OK);
    /* NULL: the parent's own, counted here. */
    extern char **environ;
    CHECK(unsetenv("MAELYS_SYS_TEST_VAR") == 0);
    int count = 0;
    for (char **entry = environ; *entry; ++entry) ++count;
    char expected[64];
    snprintf(expected, sizeof(expected), "env:%d var:no", count);
    CHECK(maelys_sys_pipe_cloexec(out) == MAELYS_SYS_OK);
    table[0].source = out[1];
    launch_t inherited = {.mode = "env", .fds = table, .fd_count = 1};
    CHECK(start(&inherited, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&out[1]) == MAELYS_SYS_OK);
    CHECK(read_all(out[0], seen, sizeof(seen)) == 0);
    CHECK_SEEN(seen, expected);
    CHECK(finish(&process, 0) == 0);
    CHECK(maelys_sys_fd_close(&out[0]) == MAELYS_SYS_OK);
    return 0;
}

static int where(const char *cwd, unsigned flags, char *seen, size_t capacity) {
    int out[2];
    maelys_sys_process_t *process = NULL;
    CHECK(maelys_sys_pipe_cloexec(out) == MAELYS_SYS_OK);
    maelys_sys_process_fd_t table[1] = {{out[1], 1}};
    launch_t launch = {.mode = "where", .cwd = cwd, .fds = table, .fd_count = 1, .flags = flags};
    CHECK(start(&launch, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&out[1]) == MAELYS_SYS_OK);
    CHECK(read_all(out[0], seen, capacity) == 0);
    CHECK(finish(&process, 0) == 0);
    CHECK(maelys_sys_fd_close(&out[0]) == MAELYS_SYS_OK);
    return 0;
}

static int test_cwd_session_group(void) {
    char seen[4096 + 64], expected[4096 + 64], resolved[4096];
    CHECK(realpath(root, resolved) != NULL);
    CHECK(where(root, 0, seen, sizeof(seen)) == 0);
    snprintf(expected, sizeof(expected), "cwd:%s sid:0 pgid:0", resolved);
    CHECK_SEEN(seen, expected);
    CHECK(where(NULL, MAELYS_SYS_PROCESS_NEW_SESSION, seen, sizeof(seen)) == 0);
    CHECK(strstr(seen, " sid:1 pgid:1") != NULL);
    CHECK(where(NULL, MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP, seen, sizeof(seen)) == 0);
    CHECK(strstr(seen, " sid:0 pgid:1") != NULL);
    return 0;
}

/* A program that sleeps: a bounded wait says so, a signal ends it, and
 * what is reaped is reaped once. */
static int test_wait_and_signal(void) {
    maelys_sys_process_t *process = NULL;
    maelys_sys_process_status_t status;
    uint64_t deadline = 0;
    launch_t launch = {.mode = "sleep", .arguments = {"10000"}};
    CHECK(start(&launch, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_pid(process) > 0 && maelys_sys_process_exit_fd(process) >= 0);
    CHECK(maelys_sys_deadline_after(100, &deadline) == MAELYS_SYS_OK);
    uint64_t began = now_ms();
    CHECK(maelys_sys_process_wait(process, deadline, &status) == MAELYS_SYS_ERR_TIMEOUT);
    CHECK(now_ms() - began >= 90u);
    CHECK(maelys_sys_process_wait(process, MAELYS_SYS_DEADLINE_INFINITE, &status) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_process_signal(process, SIGTERM) == MAELYS_SYS_OK);
    CHECK(maelys_sys_deadline_after(1000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_wait(process, deadline, &status) == MAELYS_SYS_OK);
    CHECK(status.signaled && status.term_signal == SIGTERM && !status.exited);
    CHECK(maelys_sys_process_wait(process, deadline, &status) == MAELYS_SYS_ERR_STATE);
    CHECK(maelys_sys_process_signal(process, SIGTERM) == MAELYS_SYS_ERR_STATE);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK && !process);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_release(NULL) == MAELYS_SYS_OK);
    /* An unknown signal is the caller's mistake, not the host's. */
    CHECK(start(&launch, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_signal(process, 100000) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_process_terminate(process, 0, 1000, &status) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    return 0;
}

/* The ladder against a program that ignores SIGTERM: ended by SIGKILL
 * after the grace, within the bound. */
static int test_terminate_ladder(void) {
    maelys_sys_process_t *process = NULL;
    maelys_sys_process_status_t status;
    int ready[2];
    CHECK(maelys_sys_pipe_cloexec(ready) == MAELYS_SYS_OK);
    maelys_sys_process_fd_t table[1] = {{ready[1], 1}};
    launch_t launch = {.mode = "ignore-term", .fds = table, .fd_count = 1};
    CHECK(start(&launch, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&ready[1]) == MAELYS_SYS_OK);
    char word[8];
    CHECK(read(ready[0], word, sizeof(word)) == 5); /* "ready": the handler is in place */
    CHECK(maelys_sys_fd_close(&ready[0]) == MAELYS_SYS_OK);
    uint64_t began = now_ms();
    CHECK(maelys_sys_process_terminate(process, 200, 1000, &status) == MAELYS_SYS_OK);
    uint64_t took = now_ms() - began;
    CHECK(status.signaled && status.term_signal == SIGKILL);
    CHECK(took >= 190u && took < 1600u);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    /* A program that heeds SIGTERM ends on the first rung. */
    launch_t polite = {.mode = "sleep", .arguments = {"10000"}};
    CHECK(start(&polite, &process) == MAELYS_SYS_OK);
    began = now_ms();
    CHECK(maelys_sys_process_terminate(process, 2000, 1000, &status) == MAELYS_SYS_OK);
    CHECK(status.signaled && status.term_signal == SIGTERM && now_ms() - began < 1500u);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    return 0;
}

/* A program already ended: terminate is immediate and keeps its status,
 * reaped already or not. */
static int test_terminate_already_ended(void) {
    maelys_sys_process_t *process = NULL;
    maelys_sys_process_status_t status;
    uint64_t deadline = 0;
    launch_t launch = {.mode = "exit", .arguments = {"7"}};
    CHECK(start(&launch, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_deadline_after(3000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_wait(process, deadline, &status) == MAELYS_SYS_OK);
    CHECK(status.exited && status.exit_code == 7);
    memset(&status, 0, sizeof(status));
    uint64_t began = now_ms();
    CHECK(maelys_sys_process_terminate(process, 500, 500, &status) == MAELYS_SYS_OK);
    CHECK(status.exited && status.exit_code == 7 && now_ms() - began < 100u);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    /* Ended, not reaped yet. */
    launch_t other = {.mode = "exit", .arguments = {"3"}};
    CHECK(start(&other, &process) == MAELYS_SYS_OK);
    CHECK(ended_unreaped(maelys_sys_process_pid(process)));
    began = now_ms();
    CHECK(maelys_sys_process_terminate(process, 500, 500, &status) == MAELYS_SYS_OK);
    CHECK(status.exited && status.exit_code == 3 && now_ms() - began < 100u);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    return 0;
}

/* The end of a program is a loop event, with nothing polled: the loop
 * sleeps until the descriptor says so. A program that ends before the
 * handle is complete says so at once. */
static int test_exit_fd_in_loop(void) {
    static const char *const lengths[2] = {"300", "0"};
    for (int round = 0; round < 2; ++round) {
        maelys_sys_process_t *process = NULL;
        maelys_sys_loop_t *loop = NULL;
        maelys_sys_watch_t watch = 0;
        launch_t launch = {.mode = "sleep", .arguments = {lengths[round]}};
        CHECK(start(&launch, &process) == MAELYS_SYS_OK);
        if (round == 1) CHECK(ended_unreaped(maelys_sys_process_pid(process)));
        CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
        CHECK(maelys_sys_loop_watch_fd(loop, maelys_sys_process_exit_fd(process),
            MAELYS_SYS_INTEREST_READ, 7, &watch) == MAELYS_SYS_OK);
        maelys_sys_event_t events[2];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_STOPPED;
        uint64_t deadline = 0, began = now_ms();
        CHECK(maelys_sys_deadline_after(3000, &deadline) == MAELYS_SYS_OK);
        CHECK(maelys_sys_loop_step(loop, deadline, events, 2, &count, &step) == MAELYS_SYS_OK);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == 1);
        CHECK(events[0].token == 7 && (events[0].flags & MAELYS_SYS_EVENT_READ));
        uint64_t took = now_ms() - began;
        CHECK(round == 0 ? took >= 250u && took < 2500u : took < 500u);
        maelys_sys_process_status_t status;
        CHECK(maelys_sys_monotonic_ms(&deadline) == MAELYS_SYS_OK);
        CHECK(maelys_sys_process_wait(process, deadline, &status) == MAELYS_SYS_OK);
        CHECK(status.exited && status.exit_code == 0);
        CHECK(maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_OK);
        CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
        CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    }
    return 0;
}

/* Releasing a running program: the handle goes, the program stays, and
 * the result says so. */
static int test_release_running(void) {
    maelys_sys_process_t *process = NULL;
    launch_t launch = {.mode = "sleep", .arguments = {"10000"}};
    CHECK(start(&launch, &process) == MAELYS_SYS_OK);
    pid_t pid = maelys_sys_process_pid(process);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_ERR_STATE && !process);
    CHECK(kill(pid, 0) == 0);
    CHECK(kill(pid, SIGKILL) == 0);
    int raw = 0;
    CHECK(waitpid(pid, &raw, 0) == pid && WIFSIGNALED(raw));
    /* Ended and not reaped: release reaps on the way. */
    launch_t short_lived = {.mode = "exit", .arguments = {"0"}};
    CHECK(start(&short_lived, &process) == MAELYS_SYS_OK);
    CHECK(ended_unreaped(maelys_sys_process_pid(process)));
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK && no_child_left());
    return 0;
}

/* An identity names one process, never a namesake. */
static int test_identity(void) {
    maelys_sys_process_identity_t mine, other, zombie;
    int alive = 0;
    CHECK(maelys_sys_process_identify(getpid(), &mine) == MAELYS_SYS_OK);
    CHECK(mine.pid == getpid() && mine.boot != 0);
    CHECK(maelys_sys_process_alive(&mine, &alive) == MAELYS_SYS_OK && alive == 1);
    other = mine;
    other.birth += 1;
    CHECK(maelys_sys_process_alive(&other, &alive) == MAELYS_SYS_OK && alive == 0);
    other = mine;
    other.boot += 1;
    CHECK(maelys_sys_process_alive(&other, &alive) == MAELYS_SYS_OK && alive == 0);
    /* A refused call clears its output: not on the identity kept above. */
    CHECK(maelys_sys_process_identify(0, &other) == MAELYS_SYS_ERR_ARGUMENT && other.pid == 0);
    CHECK(maelys_sys_process_identify(getpid(), NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_process_alive(NULL, &alive) == MAELYS_SYS_ERR_ARGUMENT);

    maelys_sys_process_t *process = NULL;
    maelys_sys_process_status_t status;
    launch_t launch = {.mode = "sleep", .arguments = {"10000"}};
    CHECK(start(&launch, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_identify(maelys_sys_process_pid(process), &other) == MAELYS_SYS_OK);
    CHECK(other.boot == mine.boot);
    CHECK(maelys_sys_process_alive(&other, &alive) == MAELYS_SYS_OK && alive == 1);
    CHECK(maelys_sys_process_terminate(process, 1000, 1000, &status) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_alive(&other, &alive) == MAELYS_SYS_OK && alive == 0);
    CHECK(maelys_sys_process_identify(other.pid, &zombie) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);

    /* Ended, not reaped: the hosts differ, and the header says how. */
    launch_t short_lived = {.mode = "exit", .arguments = {"0"}};
    CHECK(start(&short_lived, &process) == MAELYS_SYS_OK);
    CHECK(ended_unreaped(maelys_sys_process_pid(process)));
    maelys_sys_result_t found = maelys_sys_process_identify(maelys_sys_process_pid(process), &zombie);
#if defined(__linux__)
    CHECK(found == MAELYS_SYS_OK);
#else
    CHECK(found == MAELYS_SYS_ERR_NOT_FOUND);
#endif
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    return 0;
}

/* The end of a process this handle did not start, as a descriptor. */
static int test_foreign_exit_fd(void) {
    maelys_sys_process_t *process = NULL;
    maelys_sys_process_identity_t identity;
    maelys_sys_process_status_t status;
    int fd = 7, before = count_open();
    launch_t launch = {.mode = "sleep", .arguments = {"10000"}};
    CHECK(start(&launch, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_identify(maelys_sys_process_pid(process), &identity) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_exit_fd_open(&identity, &fd) == MAELYS_SYS_OK && fd >= 0);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    unsigned flags = 0;
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(100, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_wait(fd, MAELYS_SYS_INTEREST_READ, deadline, &flags) == MAELYS_SYS_ERR_TIMEOUT);
    CHECK(maelys_sys_process_signal(process, SIGTERM) == MAELYS_SYS_OK);
    CHECK(maelys_sys_deadline_after(3000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_wait(fd, MAELYS_SYS_INTEREST_READ, deadline, &flags) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_wait(process, deadline, &status) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&fd) == MAELYS_SYS_OK);
    /* Reaped: no descriptor for a number that names nothing. */
    fd = 7;
    CHECK(maelys_sys_process_exit_fd_open(&identity, &fd) == MAELYS_SYS_ERR_NOT_FOUND && fd == -1);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    CHECK(count_open() == before);
    return 0;
}

/* Two hundred starts and stops beside threads that allocate without pause:
 * nothing leaks, nothing blocks, the parent's descriptors are constant. */
static atomic_int stop_allocating;

static void *allocate(void *context) {
    (void)context;
    while (!atomic_load(&stop_allocating)) {
        void *block = malloc(4096);
        if (block) {
            memset(block, 1, 4096);
            free(block);
        }
    }
    return NULL;
}

static int test_many_beside_threads(void) {
    maelys_sys_thread_t *threads[4] = {NULL, NULL, NULL, NULL};
    atomic_store(&stop_allocating, 0);
    for (int index = 0; index < 4; ++index) {
        CHECK(maelys_sys_thread_create("alloc", allocate, NULL, &threads[index]) == MAELYS_SYS_OK);
    }
    int before = count_open();
    for (int round = 0; round < 200; ++round) {
        maelys_sys_process_t *process = NULL;
        maelys_sys_process_status_t status;
        launch_t launch = {.mode = "sleep", .arguments = {"10000"}};
        CHECK(start(&launch, &process) == MAELYS_SYS_OK);
        CHECK(maelys_sys_process_terminate(process, 1000, 1000, &status) == MAELYS_SYS_OK);
        CHECK(status.signaled && status.term_signal == SIGTERM);
        CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    }
    CHECK(count_open() == before);
    atomic_store(&stop_allocating, 1);
    for (int index = 0; index < 4; ++index) {
        void *result = NULL;
        CHECK(maelys_sys_thread_join(&threads[index], &result) == MAELYS_SYS_OK);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--child") == 0) return run_child(argc - 2, argv + 2);
    if (!realpath(argv[0], self)) {
#if defined(__linux__)
        ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (length <= 0) return 1;
        self[length] = '\0';
#else
        return 1;
#endif
    }
    const char *base = getenv("TMPDIR");
    if (!base || base[0] != '/') base = "/tmp";
    int written = snprintf(root, sizeof(root), "%s/maelys-sys-process.XXXXXX", base);
    if (written <= 0 || (size_t)written >= sizeof(root) || !mkdtemp(root)) return 1;
    int before = count_open();
    int failed = test_stdio_layout() || test_isolated_layout() || test_table_as_a_whole() ||
        test_refusals() || test_start_refused() || test_environment() ||
        test_cwd_session_group() || test_wait_and_signal() || test_terminate_ladder() ||
        test_terminate_already_ended() || test_exit_fd_in_loop() || test_release_running() ||
        test_identity() || test_foreign_exit_fd() || test_many_beside_threads();
    if (failed) return 1;
    if (count_open() != before) {
        fprintf(stderr, "descriptors left open\n");
        return 1;
    }
    if (!no_child_left()) {
        fprintf(stderr, "a process left behind\n");
        return 1;
    }
    if (rmdir(root) != 0) {
        fprintf(stderr, "work directory not empty: %s\n", root);
        return 1;
    }
    puts("ok - process STDIO layout: exactly 0 1 2");
    puts("ok - process ISOLATED layout beside forty stray descriptors: exactly 0 1 2 3 4");
    puts("ok - process table applied as a whole: an exchange, one source on two targets");
    puts("ok - process refusals before any process exists");
    puts("ok - process a missing path is NOT_FOUND, a file not runnable is OS/EACCES, no process left");
    puts("ok - process explicit and inherited environments");
    puts("ok - process cwd, new session, new process group");
    puts("ok - process bounded wait, signal, reaped once");
    puts("ok - process terminate ladder: SIGTERM ignored, SIGKILL within the bound");
    puts("ok - process terminate of a program already ended keeps its status");
    puts("ok - process exit descriptor in a loop, a program ended before the handle included");
    puts("ok - process release of a running program says STATE and keeps nothing");
    puts("ok - process identity: never a namesake; ended and not reaped as the host has it");
    puts("ok - process exit descriptor of a process not started here");
    puts("ok - process two hundred starts beside allocating threads: nothing left open");
    return 0;
}
