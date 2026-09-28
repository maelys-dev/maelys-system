/*
 * Descriptor passing, black box. This test includes the public header and
 * the C library only and is linked with src/fdpass.o alone, without the
 * archive and without -pthread: that it links is the proof the unit stands
 * alone, and each check below holds on Linux and macOS.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* SCM_RIGHTS and setitimer under strict POSIX */
#else
#define _DEFAULT_SOURCE
#endif

#include "maelys/sys/fdpass.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
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

/* The most descriptors one message carries on both hosts (Linux: 253). */
#define MOST_ON_BOTH 253

static int open_limit = 1024;

static int pair(int fds[2]) {
    return socketpair(AF_UNIX, SOCK_DGRAM, 0, fds);
}

static void close_pair(int fds[2]) {
    (void)close(fds[0]);
    (void)close(fds[1]);
}

/* How many descriptors are open: a leak shows as a difference. */
static int count_open(void) {
    int count = 0;
    for (int fd = 0; fd < open_limit; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

/* A sender the primitive does not offer: several descriptors in one header. */
static int send_many(int socket_fd, int descriptor, size_t count) {
    static union {
        struct cmsghdr header;
        unsigned char space[CMSG_SPACE(sizeof(int) * MOST_ON_BOTH)];
    } control;
    int descriptors[MOST_ON_BOTH];
    for (size_t index = 0; index < count; ++index) descriptors[index] = descriptor;
    char byte = 'x';
    struct iovec iov = {&byte, 1};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    memset(&control, 0, sizeof(control));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.space;
    /* socklen_t on macOS, size_t on Linux: the value is small either way. */
    message.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * count);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * count);
    memcpy(CMSG_DATA(header), descriptors, sizeof(int) * count);
    return sendmsg(socket_fd, &message, 0) == 1 ? 0 : -1;
}

static int same_file(int left, int right) {
    struct stat a, b;
    return fstat(left, &a) == 0 && fstat(right, &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

static int test_round_trip(void) {
    int fds[2], carried[2];
    CHECK(pair(fds) == 0 && pipe(carried) == 0);
    int before = count_open();
    CHECK(maelys_sys_fd_send(fds[0], "hello", 5u, carried[0]) == MAELYS_SYS_OK);
    /* The sender keeps its descriptor: the receiver gets a copy. */
    CHECK(fcntl(carried[0], F_GETFD) >= 0);
    char buffer[16];
    int received[2];
    size_t length = 0, count = 0;
    unsigned flags = 0;
    CHECK(maelys_sys_fd_receive(fds[1], buffer, sizeof(buffer), &length,
        received, 2u, &count, &flags) == MAELYS_SYS_OK);
    CHECK(length == 5u && memcmp(buffer, "hello", 5u) == 0);
    CHECK(count == 1u && flags == 0u && received[1] == -1);
    CHECK(received[0] != carried[0] && same_file(received[0], carried[0]));
    CHECK(fcntl(received[0], F_GETFD) & FD_CLOEXEC);
    CHECK(count_open() == before + 1);
    CHECK(close(received[0]) == 0);
    CHECK(count_open() == before);
    CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
    close_pair(fds);
    return 0;
}

static int test_empty_datagrams(void) {
    int fds[2], carried[2];
    CHECK(pair(fds) == 0 && pipe(carried) == 0);
    int received[1];
    size_t length = 1, count = 1;
    unsigned flags = 1;
    /* An empty datagram with a descriptor, then one without. */
    CHECK(maelys_sys_fd_send(fds[0], NULL, 0u, carried[1]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_receive(fds[1], NULL, 0u, &length, received, 1u, &count,
        &flags) == MAELYS_SYS_OK);
    CHECK(length == 0u && count == 1u && flags == 0u && same_file(received[0], carried[1]));
    CHECK(close(received[0]) == 0);
    CHECK(maelys_sys_fd_send(fds[0], NULL, 0u, -1) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_receive(fds[1], NULL, 0u, &length, NULL, 0u, &count,
        &flags) == MAELYS_SYS_OK);
    CHECK(length == 0u && count == 0u && flags == 0u);
    CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
    close_pair(fds);
    return 0;
}

static int test_data_truncated(void) {
    int fds[2];
    CHECK(pair(fds) == 0);
    CHECK(maelys_sys_fd_send(fds[0], "0123456789", 10u, -1) == MAELYS_SYS_OK);
    char buffer[4];
    size_t length = 0, count = 0;
    unsigned flags = 0;
    CHECK(maelys_sys_fd_receive(fds[1], buffer, sizeof(buffer), &length, NULL, 0u,
        &count, &flags) == MAELYS_SYS_OK);
    CHECK(length == 4u && memcmp(buffer, "0123", 4u) == 0);
    CHECK(flags == MAELYS_SYS_FDPASS_TRUNCATED && count == 0u);
    close_pair(fds);
    return 0;
}

/* More descriptors than the caller takes: the rest is closed here, said by
 * SURPLUS, and nothing leaks -- up to the most one message carries. */
static int test_surplus_closed(void) {
    static const size_t sent[] = {3u, 2u, MOST_ON_BOTH};
    static const size_t taken[] = {1u, 0u, 2u};
    for (size_t round = 0; round < sizeof(sent) / sizeof(sent[0]); ++round) {
        int fds[2], carried[2];
        CHECK(pair(fds) == 0 && pipe(carried) == 0);
        int before = count_open();
        CHECK(send_many(fds[0], carried[0], sent[round]) == 0);
        char byte;
        int received[2] = {-1, -1};
        size_t length = 0, count = 0;
        unsigned flags = 0;
        CHECK(maelys_sys_fd_receive(fds[1], &byte, 1u, &length,
            taken[round] ? received : NULL, taken[round], &count, &flags) ==
            MAELYS_SYS_OK);
        CHECK(length == 1u && count == taken[round]);
        CHECK(flags == MAELYS_SYS_FDPASS_SURPLUS);
        CHECK(count_open() == before + (int)taken[round]);
        for (size_t index = 0; index < count; ++index) {
            CHECK(same_file(received[index], carried[0]));
            CHECK(close(received[index]) == 0);
        }
        CHECK(count_open() == before);
        CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
        close_pair(fds);
    }
    return 0;
}

/* One code for a peer that left or reads no more, whatever the host says. */
static int test_peer_gone(void) {
    int fds[2];
    CHECK(pair(fds) == 0);
    CHECK(close(fds[1]) == 0);
    CHECK(maelys_sys_fd_send(fds[0], "x", 1u, -1) == MAELYS_SYS_ERR_CLOSED);
    CHECK(close(fds[0]) == 0);

    CHECK(pair(fds) == 0);
    CHECK(shutdown(fds[1], SHUT_RD) == 0);
    CHECK(maelys_sys_fd_send(fds[0], "x", 1u, -1) == MAELYS_SYS_ERR_CLOSED);
    close_pair(fds);

    CHECK(pair(fds) == 0);
    CHECK(shutdown(fds[0], SHUT_WR) == 0);
    CHECK(maelys_sys_fd_send(fds[0], "x", 1u, -1) == MAELYS_SYS_ERR_CLOSED);
    close_pair(fds);
    return 0;
}

static int test_would_block(void) {
    int fds[2];
    CHECK(pair(fds) == 0);
    CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0 && fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
    char buffer[8];
    size_t length = 1, count = 1;
    unsigned flags = 1;
    CHECK(maelys_sys_fd_receive(fds[1], buffer, sizeof(buffer), &length, NULL, 0u,
        &count, &flags) == MAELYS_SYS_ERR_WOULD_BLOCK);
    CHECK(length == 0u && count == 0u && flags == 0u);
    /* Fill the peer's queue: EAGAIN on Linux, ENOBUFS on macOS, one code. */
    static const char block[512];
    maelys_sys_result_t result = MAELYS_SYS_OK;
    for (int sends = 0; sends < 100000 && result == MAELYS_SYS_OK; ++sends) {
        result = maelys_sys_fd_send(fds[0], block, sizeof(block), -1);
    }
    CHECK(result == MAELYS_SYS_ERR_WOULD_BLOCK);
    close_pair(fds);
    return 0;
}

static int test_arguments(void) {
    int fds[2], carried[2];
    CHECK(pair(fds) == 0 && pipe(carried) == 0);
    char buffer[4];
    int received[2] = {7, 7};
    size_t length = 1, count = 1;
    unsigned flags = 1;
    /* Not a socket, a stream socket, an internet socket, no socket at all. */
    int stream[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
    int internet = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(internet >= 0);
    const int wrong[] = {carried[0], stream[0], internet, -1};
    for (size_t index = 0; index < sizeof(wrong) / sizeof(wrong[0]); ++index) {
        CHECK(maelys_sys_fd_send(wrong[index], "x", 1u, -1) == MAELYS_SYS_ERR_ARGUMENT);
        CHECK(maelys_sys_fd_receive(wrong[index], buffer, sizeof(buffer), &length,
            received, 2u, &count, &flags) == MAELYS_SYS_ERR_ARGUMENT);
        /* The outputs are cleared even when the call refuses. */
        CHECK(length == 0u && count == 0u && flags == 0u);
        CHECK(received[0] == -1 && received[1] == -1);
        received[0] = received[1] = 7;
        length = count = 1;
        flags = 1;
    }
    CHECK(close(stream[0]) == 0 && close(stream[1]) == 0 && close(internet) == 0);
    /* A passed descriptor below -1, or not open. */
    CHECK(maelys_sys_fd_send(fds[0], "x", 1u, -2) == MAELYS_SYS_ERR_ARGUMENT);
    int closed = dup(carried[0]);
    CHECK(closed >= 0 && close(closed) == 0);
    CHECK(maelys_sys_fd_send(fds[0], "x", 1u, closed) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_send(fds[0], NULL, 1u, -1) == MAELYS_SYS_ERR_ARGUMENT);
    /* Missing outputs, or a buffer the capacity says exists. */
    CHECK(maelys_sys_fd_receive(fds[1], buffer, sizeof(buffer), NULL, NULL, 0u,
        &count, &flags) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_receive(fds[1], buffer, sizeof(buffer), &length, NULL, 0u,
        NULL, &flags) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_receive(fds[1], buffer, sizeof(buffer), &length, NULL, 0u,
        &count, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_receive(fds[1], NULL, 4u, &length, NULL, 0u, &count,
        &flags) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_receive(fds[1], buffer, sizeof(buffer), &length, NULL, 2u,
        &count, &flags) == MAELYS_SYS_ERR_ARGUMENT);
    /* Nothing refused was sent. */
    CHECK(fcntl(fds[1], F_SETFL, O_NONBLOCK) == 0);
    CHECK(maelys_sys_fd_receive(fds[1], buffer, sizeof(buffer), &length, NULL, 0u,
        &count, &flags) == MAELYS_SYS_ERR_WOULD_BLOCK);
    CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
    close_pair(fds);
    return 0;
}

static void on_alarm(int signal_number) {
    (void)signal_number;
}

/* A signal without SA_RESTART interrupts the blocking receive, which must
 * resume and return the datagram a child sends after it. */
static int test_eintr_resumes(void) {
    int fds[2];
    CHECK(pair(fds) == 0);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_alarm;
    CHECK(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, NULL) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct timespec pause = {0, 300000000};
        (void)nanosleep(&pause, NULL);
        _exit(maelys_sys_fd_send(fds[0], "late", 4u, -1) == MAELYS_SYS_OK ? 0 : 1);
    }
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = 50000;
    CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
    char buffer[8];
    size_t length = 0, count = 0;
    unsigned flags = 0;
    maelys_sys_result_t result = maelys_sys_fd_receive(fds[1], buffer, sizeof(buffer),
        &length, NULL, 0u, &count, &flags);
    memset(&timer, 0, sizeof(timer));
    (void)setitimer(ITIMER_REAL, &timer, NULL);
    (void)signal(SIGALRM, SIG_IGN);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(result == MAELYS_SYS_OK && length == 4u && memcmp(buffer, "late", 4u) == 0);
    close_pair(fds);
    return 0;
}

int main(void) {
    /* The largest round receives 253 descriptors at once; macOS starts
     * processes with a soft limit of 256. */
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        rlim_t wanted = 1024;
        if (limit.rlim_max != RLIM_INFINITY && limit.rlim_max < wanted) wanted = limit.rlim_max;
        if (limit.rlim_cur < wanted) {
            limit.rlim_cur = wanted;
            (void)setrlimit(RLIMIT_NOFILE, &limit);
        }
        if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY &&
            limit.rlim_cur < 1024) {
            open_limit = (int)limit.rlim_cur;
        }
    }
    if (test_round_trip() || test_empty_datagrams() || test_data_truncated() ||
        test_surplus_closed() || test_peer_gone() || test_would_block() ||
        test_arguments() || test_eintr_resumes()) {
        return 1;
    }
    puts("ok - fdpass round trip, copy kept by the sender, close-on-exec");
    puts("ok - fdpass empty datagrams, with and without a descriptor");
    puts("ok - fdpass truncated datagram flagged");
    puts("ok - fdpass surplus closed and flagged, no leak up to 253");
    puts("ok - fdpass peer gone or not reading: one code on both hosts");
    puts("ok - fdpass would block, empty queue and full queue");
    puts("ok - fdpass arguments refused, outputs cleared");
    puts("ok - fdpass receive resumes after EINTR");
    return 0;
}
