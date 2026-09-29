#define _GNU_SOURCE
#include "maelys/sys/fdpass.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); \
    return 1; \
} } while (0)

enum { FD_LIMIT = 4096, DATA_SIZE = 1024 * 1024 };
#ifdef __APPLE__
enum { RIGHTS = 254 };
#else
enum { RIGHTS = 253 };
#endif

static int open_count(void) {
    int count = 0;
    for (int fd = 0; fd < FD_LIMIT; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

static int pair_create(int pair[2]) {
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    for (int i = 0; i < 2; ++i) CHECK(fcntl(pair[i], F_SETFL, O_NONBLOCK) == 0);
    return 0;
}

static int raw_rights(int socket_fd, int source, size_t length) {
    int rights[RIGHTS];
    for (size_t i = 0; i < RIGHTS; ++i) rights[i] = source;
    union { struct cmsghdr header; unsigned char bytes[CMSG_SPACE(sizeof(rights))]; } control;
    memset(&control, 0, sizeof(control));
    char bytes[] = "hello";
    struct iovec iov = {.iov_base = bytes, .iov_len = length};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = control.bytes; message.msg_controllen = sizeof(control.bytes);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(rights));
    memcpy(CMSG_DATA(header), rights, sizeof(rights));
    CHECK(sendmsg(socket_fd, &message, 0) == (ssize_t)length);
    return 0;
}

static int round_trip_and_arguments(void) {
    int pair[2]; CHECK(pair_create(pair) == 0);
    int source = open("/dev/null", O_RDONLY); CHECK(source >= 0);
    size_t sent = 99, received = 99, count = 99;
    unsigned flags = 99;
    int fd = 99;
    char byte;
    int original_flags = fcntl(pair[0], F_GETFL);
    CHECK(fcntl(pair[0], F_SETFL, original_flags & ~O_NONBLOCK) == 0);
    CHECK(maelys_sys_fd_stream_send(pair[0], "x", 1, source, &sent) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(sent == 0 && !(fcntl(pair[0], F_GETFL) & O_NONBLOCK));
    CHECK(maelys_sys_fd_stream_receive(pair[0], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(received == 0 && fd == -1 && count == 0 && flags == 0);
    CHECK(fcntl(pair[0], F_SETFL, original_flags) == 0);
    CHECK(maelys_sys_fd_stream_send(pair[0], NULL, 0, source, &sent) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(sent == 0);
    CHECK(maelys_sys_fd_stream_send(pair[0], NULL, 0, -1, &sent) == MAELYS_SYS_OK && sent == 0);
    CHECK(maelys_sys_fd_stream_send(pair[0], NULL, 1, -1, &sent) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_stream_send(pair[0], "x", 1, -2, &sent) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_stream_send(pair[0], "x", 1, -1, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 0, &received, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_stream_receive(pair[1], NULL, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, NULL, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, NULL, &flags) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, NULL, 1, &count, &flags) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_WOULD_BLOCK);
    CHECK(received == 0 && fd == -1 && count == 0 && flags == 0);
    CHECK(maelys_sys_fd_stream_send(pair[0], "ab", 2, source, &sent) == MAELYS_SYS_OK && sent == 2);
    CHECK(maelys_sys_fd_stream_send(pair[0], "cd", 2, source, &sent) == MAELYS_SYS_OK && sent == 2);
    for (int i = 0; i < 4; ++i) {
        CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_OK);
        CHECK(byte == "abcd"[i] && received == 1 && flags == 0);
        CHECK(count == (i % 2 == 0 ? 1u : 0u));
        if (count) { CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC); CHECK(close(fd) == 0); }
    }
    CHECK(fcntl(source, F_GETFD) >= 0 && fcntl(pair[0], F_GETFL) == original_flags);
    CHECK(close(pair[0]) == 0);
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_CLOSED);
    CHECK(received == 0 && count == 0 && flags == 0);
    CHECK(maelys_sys_fd_stream_send(pair[1], "x", 1, source, &sent) == MAELYS_SYS_ERR_CLOSED);
    CHECK(sent == 0 && fcntl(source, F_GETFD) >= 0);
    CHECK(close(pair[1]) == 0 && close(source) == 0);
    int datagram[2]; CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, datagram) == 0);
    CHECK(fcntl(datagram[0], F_SETFL, O_NONBLOCK) == 0);
    CHECK(maelys_sys_fd_stream_send(datagram[0], "x", 1, -1, &sent) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(close(datagram[0]) == 0 && close(datagram[1]) == 0);
    return 0;
}

static int flood_and_control_only(int credentials) {
    int pair[2]; CHECK(pair_create(pair) == 0);
#ifdef SO_PASSCRED
    if (credentials) CHECK(setsockopt(pair[1], SOL_SOCKET, SO_PASSCRED, &credentials, sizeof(credentials)) == 0);
#else
    (void)credentials;
#endif
    int source = open("/dev/null", O_RDONLY); CHECK(source >= 0);
    int baseline = open_count();
    for (int round = 0; round < 2; ++round) {
        CHECK(raw_rights(pair[0], source, 5) == 0);
        char bytes[5]; size_t received = 0, count = 0; unsigned flags = 0; int fd = -1;
        CHECK(maelys_sys_fd_stream_receive(pair[1], bytes, sizeof(bytes), &received,
              round ? &fd : NULL, round ? 1u : 0u, &count, &flags) == MAELYS_SYS_OK);
        CHECK(received == 5 && memcmp(bytes, "hello", 5) == 0);
        CHECK(count == (round ? 1u : 0u) && (flags & MAELYS_SYS_FDPASS_SURPLUS));
        CHECK(!(flags & MAELYS_SYS_FDPASS_CONTROL_TRUNCATED));
        CHECK(!!(flags & MAELYS_SYS_FDPASS_UNEXPECTED_CONTROL) == !!credentials);
        if (round) { CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC); CHECK(close(fd) == 0); }
        CHECK(open_count() == baseline);
    }
#ifdef __APPLE__
    CHECK(raw_rights(pair[0], source, 0) == 0);
    char byte; size_t received = 99, count = 99; unsigned flags = 99;
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, NULL, 0, &count, &flags) == MAELYS_SYS_OK);
    CHECK(received == 0 && count == 0 && (flags & MAELYS_SYS_FDPASS_SURPLUS));
    CHECK(open_count() == baseline);
#endif
    CHECK(shutdown(pair[0], SHUT_WR) == 0);
    char buffer; size_t n = 99, c = 99; unsigned f = 99;
    CHECK(maelys_sys_fd_stream_receive(pair[1], &buffer, 1, &n, NULL, 0, &c, &f) == MAELYS_SYS_ERR_CLOSED);
    CHECK(n == 0 && c == 0 && f == 0 && open_count() == baseline);
    CHECK(close(source) == 0 && close(pair[0]) == 0 && close(pair[1]) == 0);
    return 0;
}

static int partial_and_full_queue(void) {
    int pair[2]; CHECK(pair_create(pair) == 0);
    int bound = 4096;
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &bound, sizeof(bound)) == 0);
    int source = open("/dev/null", O_RDONLY); CHECK(source >= 0);
    int baseline = open_count();
    char *data = malloc(DATA_SIZE); CHECK(data != NULL);
    memset(data, 'x', DATA_SIZE);
    size_t queued = 0, sent = 0;
    for (;;) {
        maelys_sys_result_t result = maelys_sys_fd_stream_send(pair[0], data, DATA_SIZE, -1, &sent);
        if (result == MAELYS_SYS_ERR_WOULD_BLOCK) { CHECK(sent == 0); break; }
        CHECK(result == MAELYS_SYS_OK && sent > 0);
        queued += sent; CHECK(queued < 16u * DATA_SIZE);
    }
    CHECK(maelys_sys_fd_stream_send(pair[0], data, 16, source, &sent) == MAELYS_SYS_ERR_WOULD_BLOCK);
    CHECK(sent == 0);
    char buffer[8192]; size_t received, count; unsigned flags; int fd;
    while (queued) {
        CHECK(maelys_sys_fd_stream_receive(pair[1], buffer, sizeof(buffer), &received, &fd, 1, &count, &flags) == MAELYS_SYS_OK);
        CHECK(received > 0 && received <= queued && count == 0 && flags == 0);
        queued -= received;
    }
    size_t total_sent = 0, total_received = 0, rights = 0;
    int first = 1;
    while (total_sent < DATA_SIZE) {
        CHECK(maelys_sys_fd_stream_send(pair[0], data + total_sent, DATA_SIZE - total_sent,
              first ? source : -1, &sent) == MAELYS_SYS_OK);
        CHECK(sent > 0 && (total_sent || sent < DATA_SIZE));
        first = 0; total_sent += sent;
        while (total_received < total_sent) {
            CHECK(maelys_sys_fd_stream_receive(pair[1], buffer, sizeof(buffer), &received, &fd, 1, &count, &flags) == MAELYS_SYS_OK);
            CHECK(received > 0 && received <= total_sent - total_received && flags == 0);
            for (size_t i = 0; i < received; ++i) CHECK(buffer[i] == 'x');
            total_received += received; rights += count;
            if (count) { CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC); CHECK(close(fd) == 0); }
        }
    }
    CHECK(total_received == DATA_SIZE && rights == 1 && open_count() == baseline);
    free(data);
    CHECK(close(source) == 0 && close(pair[0]) == 0 && close(pair[1]) == 0);
    return 0;
}

static int near_full_queue(void) {
#ifdef __APPLE__
    const size_t gaps[] = {0, 1, 15, 16, 17, 24, 31, 32, 33};
    for (size_t test = 0; test < sizeof(gaps) / sizeof(gaps[0]); ++test) {
        int pair[2]; CHECK(pair_create(pair) == 0);
        int bound = 4096;
        CHECK(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &bound, sizeof(bound)) == 0);
        CHECK(setsockopt(pair[1], SOL_SOCKET, SO_RCVBUF, &bound, sizeof(bound)) == 0);
        size_t queued = 0, sent = 0;
        for (;;) {
            maelys_sys_result_t result = maelys_sys_fd_stream_send(pair[0], "x", 1, -1, &sent);
            if (result == MAELYS_SYS_ERR_WOULD_BLOCK) break;
            CHECK(result == MAELYS_SYS_OK && sent == 1 && ++queued <= 8192);
        }
        CHECK(queued >= gaps[test]);
        char bytes[8192]; size_t received = 0, count = 0; unsigned flags = 0; int fd = -1;
        if (gaps[test]) {
            CHECK(maelys_sys_fd_stream_receive(pair[1], bytes, gaps[test], &received, NULL, 0, &count, &flags) == MAELYS_SYS_OK);
            CHECK(received == gaps[test] && count == 0 && flags == 0);
            queued -= received;
        }
        int source = open("/dev/null", O_RDONLY); CHECK(source >= 0);
        int baseline = open_count();
        maelys_sys_result_t result = maelys_sys_fd_stream_send(pair[0], "0123456789abcdef", 16, source, &sent);
        CHECK((result == MAELYS_SYS_OK && sent > 0 && sent <= 16) ||
              (result == MAELYS_SYS_ERR_WOULD_BLOCK && sent == 0));
        size_t expected_rights = sent ? 1u : 0u, rights = 0;
        queued += sent;
        while (queued) {
            CHECK(maelys_sys_fd_stream_receive(pair[1], bytes, sizeof(bytes), &received, &fd, 1, &count, &flags) == MAELYS_SYS_OK);
            CHECK(received > 0 && received <= queued && flags == 0);
            queued -= received; rights += count;
            if (count) CHECK(close(fd) == 0);
        }
        CHECK(rights == expected_rights && open_count() == baseline);
        CHECK(close(source) == 0 && close(pair[0]) == 0 && close(pair[1]) == 0);
    }
#endif
    return 0;
}

int main(void) {
    struct rlimit limit;
    CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    if (limit.rlim_cur > FD_LIMIT) limit.rlim_cur = FD_LIMIT;
    CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    (void)signal(SIGPIPE, SIG_DFL);
    (void)alarm(15);
    int baseline = open_count();
    CHECK(round_trip_and_arguments() == 0);
    CHECK(flood_and_control_only(0) == 0);
#ifdef SO_PASSCRED
    CHECK(flood_and_control_only(1) == 0);
#endif
    CHECK(partial_and_full_queue() == 0);
    CHECK(near_full_queue() == 0);
    CHECK(open_count() == baseline);
    puts("stream fdpass: ok");
    return 0;
}
