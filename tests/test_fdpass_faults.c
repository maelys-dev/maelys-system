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

static int fdpass_fault(const char *step) {
    if (!fault_step || strcmp(step, fault_step) != 0) return 0;
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

static int send_three(int socket_fd, int descriptor) {
    int descriptors[3] = {descriptor, descriptor, descriptor};
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
    message.msg_controllen = sizeof(control.space);
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(descriptors));
    memcpy(CMSG_DATA(header), descriptors, sizeof(descriptors));
    return sendmsg(socket_fd, &message, 0) == 1 ? 0 : -1;
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

int main(void) {
    if (test_control_truncated() || test_fcntl_branch_sets_cloexec() ||
        test_fcntl_failure_closes_all()) {
        return 1;
    }
    puts("ok - fdpass control truncation flagged, only copied descriptors returned");
    puts("ok - fdpass fcntl branch sets close-on-exec on every host");
    puts("ok - fdpass fcntl failure closes every descriptor received");
    return 0;
}
