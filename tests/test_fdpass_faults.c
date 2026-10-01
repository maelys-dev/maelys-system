/*
 * Descriptor passing, white box: src/fdpass.c is compiled into this unit
 * with its control buffer shrunk to one descriptor, its fcntl branch forced
 * on every host and a fault point before that fcntl. So Linux runs the
 * branch macOS runs, and both reach what the full-size buffer makes
 * unreachable: the kernel truncating the control data. On macOS it then
 * installs every descriptor sent and announces in cmsg_len more than it
 * copied; the unit must read only what was copied.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif

#define MAELYS_SYS_FDPASS_TESTING 1
#define MAELYS_SYS_FDPASS_CONTROL_FDS 1
#define MAELYS_SYS_FDPASS_FORCE_FCNTL 1
#include "src/fdpass_internal.h"
#include "src/fdpass.c"

#include <stdio.h>
#include <sys/stat.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s (errno %d: %s)\n", \
            __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return 1; \
    } \
} while (0)

#define DESCRIPTOR_SCAN 1024

static const char *fault_step;
static int fault_errno;
/* How many matching steps to let through before the fault fires. */
static int fault_skip;

static int fdpass_fault(const char *step) {
    if (!fault_step || strcmp(step, fault_step) != 0) return 0;
    if (fault_skip > 0) { --fault_skip; return 0; }
    fault_step = NULL;
    errno = fault_errno;
    return 1;
}

static unsigned char open_before[DESCRIPTOR_SCAN];

static void snapshot_open(void) {
    for (int fd = 0; fd < DESCRIPTOR_SCAN; ++fd) {
        open_before[fd] = fcntl(fd, F_GETFD) >= 0;
    }
}

/* Closes what was opened since the snapshot, including descriptors macOS
 * installed without telling anyone, and says how many there were. */
static int close_since_snapshot(void) {
    int closed = 0;
    for (int fd = 0; fd < DESCRIPTOR_SCAN; ++fd) {
        if (!open_before[fd] && fcntl(fd, F_GETFD) >= 0) {
            (void)close(fd);
            ++closed;
        }
    }
    return closed;
}

static int send_many(int socket_fd, int descriptor, size_t count) {
    int descriptors[128];
    for (size_t i = 0; i < count; ++i) descriptors[i] = descriptor;
    union {
        struct cmsghdr header;
        unsigned char space[CMSG_SPACE(sizeof(descriptors))];
    } control;
    memset(&control, 0, sizeof(control));
    char byte = 'x';
    struct iovec iov = {&byte, 1};
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.space;
    message.msg_controllen = (socklen_t)CMSG_SPACE(count * sizeof(int));
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = (socklen_t)CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(header), descriptors, count * sizeof(int));
    return sendmsg(socket_fd, &message, 0) == 1 ? 0 : -1;
}

static int send_three(int socket_fd, int descriptor) {
    return send_many(socket_fd, descriptor, 3);
}

static int same_file(int left, int right) {
    struct stat a, b;
    return fstat(left, &a) == 0 && fstat(right, &b) == 0 &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

/* Control truncated: flagged, and only descriptors actually copied are
 * returned, each a copy of the one sent. */
static int test_control_truncated(void) {
    int fds[2], carried[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0 && pipe(carried) == 0);
    snapshot_open();
    CHECK(send_three(fds[0], carried[0]) == 0);
    char byte;
    int received[3] = {-1, -1, -1};
    size_t length = 0, count = 0;
    unsigned flags = 0;
    CHECK(maelys_sys_fd_receive(fds[1], &byte, 1u, &length, received, 3u, &count,
        &flags) == MAELYS_SYS_OK);
    /* What fits in one shrunk buffer after the header, alignment included. */
    size_t room = (CMSG_SPACE(sizeof(int)) - CMSG_LEN(0)) / sizeof(int);
    CHECK(flags & MAELYS_SYS_FDPASS_CONTROL_TRUNCATED);
    CHECK(count >= 1u && count <= room && count < 3u);
    for (size_t index = 0; index < count; ++index) {
        CHECK(same_file(received[index], carried[0]));
        CHECK(fcntl(received[index], F_GETFD) & FD_CLOEXEC);
    }
    for (size_t index = count; index < 3u; ++index) CHECK(received[index] == -1);
    (void)close_since_snapshot();
    CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
    CHECK(close(fds[0]) == 0 && close(fds[1]) == 0);
    return 0;
}

/* The fcntl branch sets close-on-exec where the kernel did not. */
static int test_fcntl_branch_sets_cloexec(void) {
    int fds[2], carried[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0 && pipe(carried) == 0);
    CHECK(maelys_sys_fd_send(fds[0], "x", 1u, carried[0]) == MAELYS_SYS_OK);
    char byte;
    int received = -1;
    size_t length = 0, count = 0;
    unsigned flags = 0;
    CHECK(maelys_sys_fd_receive(fds[1], &byte, 1u, &length, &received, 1u, &count,
        &flags) == MAELYS_SYS_OK);
    CHECK(count == 1u && flags == 0u && same_file(received, carried[0]));
    CHECK(fcntl(received, F_GETFD) & FD_CLOEXEC);
    CHECK(close(received) == 0);
    CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
    CHECK(close(fds[0]) == 0 && close(fds[1]) == 0);
    return 0;
}

/* That fcntl failing: every descriptor received is closed, the outputs stay
 * cleared, and the call reports the fcntl error. */
static int test_fcntl_failure_closes_all(void) {
    int fds[2], carried[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) == 0 && pipe(carried) == 0);
    CHECK(maelys_sys_fd_send(fds[0], "x", 1u, carried[0]) == MAELYS_SYS_OK);
    snapshot_open();
    fault_step = "fcntl";
    fault_errno = EMFILE;
    char byte;
    int received = 7;
    size_t length = 1, count = 1;
    unsigned flags = 1;
    errno = 0;
    CHECK(maelys_sys_fd_receive(fds[1], &byte, 1u, &length, &received, 1u, &count,
        &flags) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EMFILE);
    CHECK(received == -1 && length == 0u && count == 0u && flags == 0u);
    CHECK(close_since_snapshot() == 0);
    CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
    CHECK(close(fds[0]) == 0 && close(fds[1]) == 0);
    return 0;
}

/* Partial stream calls do not hide EINTR from the caller's deadline loop.
 * Failed receives consume nothing; a post-receive fcntl failure closes all. */
static int test_stream_faults(void) {
    int pair[2], carried[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && pipe(carried) == 0);
    CHECK(fcntl(pair[0], F_SETFL, O_NONBLOCK) == 0);
    CHECK(fcntl(pair[1], F_SETFL, O_NONBLOCK) == 0);
    size_t sent = 99, received = 99, count = 99;
    unsigned flags = 99; int fd = 99; char byte = 0;
    fault_step = "sendmsg"; fault_errno = EINTR;
    CHECK(maelys_sys_fd_stream_send(pair[0], "x", 1, carried[0], &sent) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EINTR && sent == 0);
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_WOULD_BLOCK);
    CHECK(received == 0 && fd == -1 && count == 0 && flags == 0);
    CHECK(maelys_sys_fd_stream_send(pair[0], "x", 1, carried[0], &sent) == MAELYS_SYS_OK && sent == 1);
    fault_step = "recvmsg"; fault_errno = EINTR;
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EINTR && received == 0 && fd == -1 && count == 0 && flags == 0);
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_OK);
    CHECK(received == 1 && byte == 'x' && count == 1 && flags == 0);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    CHECK(close(fd) == 0);
    CHECK(send_three(pair[0], carried[0]) == 0);
    snapshot_open();
    fault_step = "fcntl"; fault_errno = EMFILE;
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EMFILE && received == 0 && fd == -1 && count == 0 && flags == 0);
    CHECK(close_since_snapshot() == 0);
    fault_step = "recvmsg"; fault_errno = ECONNRESET;
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_ERR_RESET);
    CHECK(received == 0 && fd == -1 && count == 0 && flags == 0);
    fault_step = "sendmsg"; fault_errno = EMSGSIZE;
    CHECK(maelys_sys_fd_stream_send(pair[0], "x", 1, -1, &sent) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EMSGSIZE && sent == 0);
#ifdef __APPLE__
    fault_step = "sendmsg"; fault_errno = EMSGSIZE;
    CHECK(maelys_sys_fd_stream_send(pair[0], "x", 1, carried[0], &sent) == MAELYS_SYS_ERR_WOULD_BLOCK);
    CHECK(sent == 0);
#endif
    CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    return 0;
}

/* Marking close-on-exec failing partway through a batch, on either of its two
 * calls: every descriptor of the batch is closed, those already marked
 * included, and nothing is returned. The bytes of that read are consumed all
 * the same, which is why a stream caller abandons the exchange. And the
 * surplus is closed before any marking, so it is never marked at all. */
static int test_fcntl_failure_partway(void) {
    static const struct { const char *step; int skip; } faults[] = {
        {"fcntl", 1},        /* F_GETFD of the second descriptor */
        {"fcntl-setfd", 0},  /* F_SETFD of the first */
        {"fcntl-setfd", 2}   /* F_SETFD of the third, two already marked */
    };
    int pair[2], carried[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && pipe(carried) == 0);
    CHECK(fcntl(pair[1], F_SETFL, O_NONBLOCK) == 0);
    for (size_t round = 0; round < sizeof(faults) / sizeof(faults[0]); ++round) {
        CHECK(send_three(pair[0], carried[0]) == 0);
        snapshot_open();
        fault_step = faults[round].step;
        fault_skip = faults[round].skip;
        fault_errno = EMFILE;
        char byte = 0;
        int received[3] = {7, 7, 7};
        size_t length = 1, count = 1;
        unsigned flags = 1;
        errno = 0;
        CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &length, received, 3,
            &count, &flags) == MAELYS_SYS_ERR_OS);
        CHECK(errno == EMFILE && fault_step == NULL);
        CHECK(length == 0u && count == 0u && flags == 0u);
        CHECK(received[0] == -1 && received[1] == -1 && received[2] == -1);
        CHECK(close_since_snapshot() == 0);
        /* The byte went with the descriptors: the queue is empty again. */
        CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &length, received, 3,
            &count, &flags) == MAELYS_SYS_ERR_WOULD_BLOCK);
    }
    /* Three arrive, one is taken: only that one is marked, so a fault armed
     * for a second marking never fires and the call succeeds. */
    CHECK(send_three(pair[0], carried[0]) == 0);
    snapshot_open();
    fault_step = "fcntl";
    fault_skip = 1;
    fault_errno = EMFILE;
    char byte = 0;
    int taken = -1;
    size_t length = 0, count = 0;
    unsigned flags = 0;
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &length, &taken, 1, &count,
        &flags) == MAELYS_SYS_OK);
    CHECK(fault_step != NULL && fault_skip == 0);
    fault_step = NULL;
    CHECK(length == 1u && count == 1u && flags == MAELYS_SYS_FDPASS_SURPLUS);
    CHECK(same_file(taken, carried[0]) && (fcntl(taken, F_GETFD) & FD_CLOEXEC));
    CHECK(close(taken) == 0);
    CHECK(close_since_snapshot() == 0);
    CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    return 0;
}

static int test_stream_control_truncated(void) {
    int pair[2], carried[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && pipe(carried) == 0);
    CHECK(fcntl(pair[1], F_SETFL, O_NONBLOCK) == 0);
    snapshot_open();
    CHECK(send_many(pair[0], carried[0], 128) == 0);
    char byte; int fd = -1; size_t received = 0, count = 0; unsigned flags = 0;
    CHECK(maelys_sys_fd_stream_receive(pair[1], &byte, 1, &received, &fd, 1, &count, &flags) == MAELYS_SYS_OK);
    CHECK(received == 1 && count == 1 && same_file(fd, carried[0]));
    CHECK((flags & MAELYS_SYS_FDPASS_CONTROL_TRUNCATED) && (flags & MAELYS_SYS_FDPASS_SURPLUS));
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    /* The deliberately shrunken test buffer violates the production bound:
     * reclaim even the rights macOS installs without reporting in this case. */
    (void)close_since_snapshot();
    CHECK(close(carried[0]) == 0 && close(carried[1]) == 0);
    CHECK(close(pair[0]) == 0 && close(pair[1]) == 0);
    return 0;
}

int main(void) {
    if (test_control_truncated() || test_fcntl_branch_sets_cloexec() ||
        test_fcntl_failure_closes_all() || test_stream_faults() ||
        test_fcntl_failure_partway() || test_stream_control_truncated()) {
        return 1;
    }
    puts("ok - fdpass control truncation flagged, only copied descriptors returned");
    puts("ok - fdpass fcntl branch sets close-on-exec on every host");
    puts("ok - fdpass fcntl failure closes every descriptor received");
    puts("ok - partial stream faults preserve progress and descriptor ownership");
    puts("ok - fdpass marking failing partway closes the batch; surplus is never marked");
    return 0;
}
