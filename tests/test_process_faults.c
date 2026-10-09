/*
 * Processes, white box: src/process.c is compiled into this unit with a
 * fault point before the calls its failure paths depend on, and with the
 * Linux close_range path compiled out, so that the walk of /proc/self/fd
 * that stands in for it on an older kernel is the one that runs here. The
 * programs started are this test itself, with --child and a mode.
 */
#define MAELYS_SYS_PROCESS_TESTING 1
#define MAELYS_SYS_PROCESS_NO_CLOSE_RANGE 1
#include "src/process.c"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s (errno %d: %s)\n", \
            __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return 1; \
    } \
} while (0)

#define PATH_ROOM 512

/* A fortified realpath refuses a buffer shorter than PATH_MAX. */
static char self[4096];
static const char *fault_step;
static int fault_errno;
static int fault_skip; /* matches to let through before the step fires */
static pid_t victim;

static void sleep_ms(long ms) {
    struct timespec pause = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
}

/* Fires once for the step armed, after fault_skip matches let through.
 * Two steps act instead of failing: at "registered-late" the program is
 * waited for, unreaped, so that it has ended before its exit descriptor is
 * made; at "between" the process looked at is killed and reaped, so the
 * registration that follows finds nothing. */
static int process_fault(const char *step) {
    if (!fault_step || strcmp(step, fault_step) != 0) return 0;
    if (fault_skip > 0) {
        --fault_skip;
        return 0;
    }
    fault_step = NULL;
    if (strcmp(step, "registered-late") == 0) {
        siginfo_t info;
        while (waitid(P_PID, (id_t)process_fault_pid, &info, WEXITED | WNOWAIT) != 0 &&
            errno == EINTR) {}
        return 0;
    }
    if (strcmp(step, "between") == 0) {
        (void)kill(victim, SIGKILL);
        while (waitpid(victim, NULL, 0) < 0 && errno == EINTR) {}
        return 0;
    }
    errno = fault_errno;
    return 1;
}

static void arm(const char *step, int error) {
    fault_step = step;
    fault_errno = error;
    fault_skip = 0;
}

static int count_open(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

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

static maelys_sys_result_t start(const char *mode, const char *argument,
    const maelys_sys_process_fd_t *fds, size_t fd_count, maelys_sys_process_t **out) {
    char *argv[] = {self, "--child", (char *)mode, (char *)argument, NULL};
    maelys_sys_process_options_t options = {
        .path = self, .argv = argv, .fds = fds, .fd_count = fd_count
    };
    return maelys_sys_process_spawn(&options, out);
}

static int child_mode(const char *mode, const char *argument) {
    if (strcmp(mode, "sleep") == 0) {
        sleep_ms(atol(argument));
        return 0;
    }
    if (strcmp(mode, "exit") == 0) return atoi(argument);
    if (strcmp(mode, "fdlist") == 0) {
        /* A socket with a named peer was connected by the program itself: a
         * sanitizer runtime's log socket on macOS 15. Said as such. */
        char line[512];
        int n = snprintf(line, sizeof(line), "fds:");
        for (int fd = 0; fd < 64; ++fd) {
            struct stat status;
            struct sockaddr_un peer;
            socklen_t length = (socklen_t)sizeof(peer);
            if (fcntl(fd, F_GETFD) < 0) continue;
            memset(&peer, 0, sizeof(peer));
            if (fstat(fd, &status) == 0 && S_ISSOCK(status.st_mode) &&
                getpeername(fd, (struct sockaddr *)&peer, &length) == 0 && peer.sun_path[0]) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %d(sock:%.100s)", fd, peer.sun_path);
            } else if (fd < 3) {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %d", fd);
            } else {
                n += snprintf(line + n, sizeof(line) - (size_t)n, " %d(%s)", fd,
                    fstat(fd, &status) == 0 && S_ISSOCK(status.st_mode) ? "sock" : "other");
            }
        }
        (void)!write(1, line, (size_t)n);
        return 0;
    }
    return 99;
}

/* The creation refused, the descriptor that reports the end refused: each
 * is reported with its errno and leaves no process and nothing open. */
static int test_refused(void) {
    int before = count_open();
    maelys_sys_process_t *process = (maelys_sys_process_t *)&self;
    arm("create", EAGAIN);
    errno = 0;
    CHECK(start("sleep", "10000", NULL, 0, &process) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EAGAIN && !process && fault_step == NULL);
    CHECK(no_child_left() && count_open() == before);
#if defined(__linux__)
    arm("pipe", EMFILE);
    errno = 0;
    CHECK(start("sleep", "10000", NULL, 0, &process) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EMFILE && !process && fault_step == NULL);
    CHECK(no_child_left() && count_open() == before);
#endif
    /* The program started and cannot be followed: it is stopped and reaped
     * behind the error, so that no process outlives a refusal. */
    arm("exit-fd", EMFILE);
    errno = 0;
    CHECK(start("sleep", "10000", NULL, 0, &process) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EMFILE && !process && fault_step == NULL);
    CHECK(no_child_left() && count_open() == before);
    arm("exit-fd", ENOSYS);
    CHECK(start("sleep", "10000", NULL, 0, &process) == MAELYS_SYS_ERR_UNSUPPORTED);
    CHECK(!process && no_child_left() && count_open() == before);
    return 0;
}

/* A program that ends before its exit descriptor is made: the descriptor
 * is readable all the same, at once, on both hosts. */
static int test_ended_before_registration(void) {
    maelys_sys_process_t *process = NULL;
    maelys_sys_process_status_t status;
    uint64_t now = 0;
    arm("registered-late", 0);
    CHECK(start("exit", "5", NULL, 0, &process) == MAELYS_SYS_OK && fault_step == NULL);
    struct pollfd readable = {maelys_sys_process_exit_fd(process), POLLIN, 0};
    CHECK(poll(&readable, 1, 0) == 1 && (readable.revents & POLLIN));
    CHECK(maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_wait(process, now, &status) == MAELYS_SYS_OK);
    CHECK(status.exited && status.exit_code == 5);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    return 0;
}

/* A process that dies between the identity check and the registration,
 * and one whose identity no longer matches after it: no descriptor. */
static int test_exit_fd_open_races(void) {
    int before = count_open();
    maelys_sys_process_identity_t identity;
    int fd = 7;
    victim = fork();
    CHECK(victim >= 0);
    if (victim == 0) {
        sleep_ms(10000);
        _exit(0);
    }
    CHECK(maelys_sys_process_identify(victim, &identity) == MAELYS_SYS_OK);
    arm("between", 0);
    CHECK(maelys_sys_process_exit_fd_open(&identity, &fd) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(fd == -1 && fault_step == NULL && count_open() == before);
    CHECK(no_child_left());
    victim = fork();
    CHECK(victim >= 0);
    if (victim == 0) {
        sleep_ms(10000);
        _exit(0);
    }
    CHECK(maelys_sys_process_identify(victim, &identity) == MAELYS_SYS_OK);
    /* The identity read after the registration differs from the one read
     * before it: the first look passes, the second is answered wrongly. */
    arm("identify", 0);
    fault_skip = 1;
    CHECK(maelys_sys_process_exit_fd_open(&identity, &fd) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(fd == -1 && fault_step == NULL && count_open() == before);
    CHECK(kill(victim, SIGKILL) == 0 && waitpid(victim, NULL, 0) == victim);
    return 0;
}

/* The ISOLATED layout through the walk of /proc/self/fd, beside forty
 * stray descriptors: the same answer as through close_range. */
static int test_layout_without_close_range(void) {
    int stray[40];
    for (int index = 0; index < 40; ++index) {
        stray[index] = open("/dev/null", O_RDONLY);
        CHECK(stray[index] >= 0);
    }
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int out[2], pair[2];
    CHECK(devnull >= 0 && pipe(out) == 0 && socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    maelys_sys_process_fd_t table[4] = {{devnull, 0}, {out[1], 2}, {out[1], 1}, {pair[1], 3}};
    maelys_sys_process_t *process = NULL;
    CHECK(start("fdlist", "-", table, 4, &process) == MAELYS_SYS_OK);
    CHECK(close(out[1]) == 0);
    char seen[256];
    size_t length = 0;
    for (;;) {
        ssize_t got = read(out[0], seen + length, sizeof(seen) - 1 - length);
        if (got < 0 && errno == EINTR) continue;
        CHECK(got >= 0);
        if (got == 0) break;
        length += (size_t)got;
    }
    seen[length] = '\0';
    /* A socket the program connected itself, on a number the table (0 to 3
     * here) does not name, is not one of the table's: set apart. */
    char *cursor = seen, *entry;
    while ((entry = strstr(cursor, "(sock:")) != NULL) {
        char *start = entry, *end = strchr(entry, ')');
        while (start > seen && start[-1] != ' ') --start;
        if (!end) break;
        if (atoi(start) <= 3) {
            cursor = end;
            continue;
        }
        fprintf(stderr, "note: the program opened %.*s itself; set apart\n", (int)(end + 1 - start), start);
        memmove(start > seen ? start - 1 : start, end + 1, strlen(end + 1) + 1);
        cursor = start > seen ? start - 1 : start;
    }
    if (strcmp(seen, "fds: 0 1 2 3(sock)") != 0) {
        fprintf(stderr, "the child saw \"%s\"\n", seen);
        return 1;
    }
    maelys_sys_process_status_t status;
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(3000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_wait(process, deadline, &status) == MAELYS_SYS_OK && status.exited);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    for (int index = 0; index < 40; ++index) CHECK(close(stray[index]) == 0);
    CHECK(close(devnull) == 0 && close(out[0]) == 0 && close(pair[0]) == 0 && close(pair[1]) == 0);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "--child") == 0) return child_mode(argv[2], argv[3]);
    if (!realpath(argv[0], self)) return 1;
    int before = count_open();
    if (test_refused() || test_ended_before_registration() || test_exit_fd_open_races() ||
        test_layout_without_close_range()) return 1;
    if (count_open() != before || !no_child_left()) {
        fprintf(stderr, "descriptors or a process left behind\n");
        return 1;
    }
    puts("ok - process refused creation or exit descriptor: errno, no process, nothing open");
    puts("ok - process ended before its exit descriptor is made: readable at once");
    puts("ok - process exit descriptor of a process that died or changed hands: none");
#if defined(__linux__)
    puts("ok - process ISOLATED layout through the walk of /proc/self/fd");
#else
    puts("ok - process ISOLATED layout, close_range being Linux's");
#endif
    return 0;
}
