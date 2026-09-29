#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "maelys/sys/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #x, errno); \
    return 1; \
} } while (0)

enum { FD_LIMIT = 4096 };
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

static int send_rights(int socket_fd, int source, size_t count, size_t length) {
    int fds[RIGHTS];
    for (size_t i = 0; i < count; ++i) fds[i] = source;
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(fds))];
    } control;
    memset(&control, 0, sizeof(control));
    char payload[] = "hello";
    struct iovec iov = { .iov_base = payload, .iov_len = length };
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = (socklen_t)CMSG_SPACE(count * sizeof(int));
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = (socklen_t)CMSG_LEN(count * sizeof(int));
    memcpy(CMSG_DATA(header), fds, count * sizeof(int));
    CHECK(sendmsg(socket_fd, &message, 0) == (ssize_t)length);
    return 0;
}

static int exercise(maelys_sys_socket_t *receiver, int sender, int source) {
    char buffer[32];
    size_t received = 99;
    int baseline = open_count();
    CHECK(maelys_sys_socket_receive(receiver, buffer, sizeof(buffer), &received) ==
          MAELYS_SYS_ERR_WOULD_BLOCK);
    CHECK(received == 0);
    /* Repeat at each kernel's per-message maximum. Reads of one byte must
     * dispose of all rights even when payload remains in the socket. */
    for (int round = 0; round < 8; ++round) {
        CHECK(send_rights(sender, source, RIGHTS, 5) == 0);
        for (size_t i = 0; i < 5; ++i) {
            CHECK(maelys_sys_socket_receive(receiver, buffer, 1, &received) == MAELYS_SYS_OK);
            CHECK(received == 1 && buffer[0] == "hello"[i]);
            CHECK(open_count() == baseline);
        }
    }
    /* Adjacent rights-bearing sends, with a receive large enough to cross
     * their boundary. The kernel's control segmentation must not leak. */
    CHECK(send_rights(sender, source, RIGHTS, 5) == 0);
    CHECK(send_rights(sender, source, RIGHTS, 5) == 0);
    size_t total = 0;
    while (total < 10) {
        CHECK(maelys_sys_socket_receive(receiver, buffer + total, 10 - total, &received) == MAELYS_SYS_OK);
        CHECK(received > 0);
        total += received;
        CHECK(open_count() == baseline);
    }
    CHECK(memcmp(buffer, "hellohello", 10) == 0);
    /* macOS delivers control without data; Linux discards it. Neither is
     * an EOF, and the next ordinary byte must still be available. */
    CHECK(send_rights(sender, source, RIGHTS, 0) == 0);
    CHECK(send(sender, "x", 1, 0) == 1);
    CHECK(maelys_sys_socket_receive(receiver, buffer, sizeof(buffer), &received) == MAELYS_SYS_OK);
    CHECK(received == 1 && buffer[0] == 'x');
    CHECK(open_count() == baseline);
    CHECK(send_rights(sender, source, RIGHTS, 0) == 0);
    CHECK(maelys_sys_socket_receive(receiver, buffer, sizeof(buffer), &received) == MAELYS_SYS_ERR_WOULD_BLOCK);
    CHECK(received == 0 && open_count() == baseline);
#ifdef __APPLE__
    /* A hostile stream of control-only records must yield an error, not
     * spin forever, fabricate EOF or announce success with zero bytes. */
    for (int i = 0; i < 16; ++i) CHECK(send_rights(sender, source, 1, 0) == 0);
    CHECK(maelys_sys_socket_receive(receiver, buffer, sizeof(buffer), &received) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EPROTO && received == 0 && open_count() == baseline);
    CHECK(send(sender, "y", 1, 0) == 1);
    CHECK(maelys_sys_socket_receive(receiver, buffer, sizeof(buffer), &received) == MAELYS_SYS_OK);
    CHECK(received == 1 && buffer[0] == 'y' && open_count() == baseline);
#endif
    CHECK(shutdown(sender, SHUT_WR) == 0);
    CHECK(maelys_sys_socket_receive(receiver, buffer, sizeof(buffer), &received) == MAELYS_SYS_ERR_CLOSED);
    CHECK(received == 0 && open_count() == baseline);
    return 0;
}

static int connected_pair(int passcred) {
    char directory[] = "/tmp/maelys-socket-rights.XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    int length = snprintf(address.sun_path, sizeof(address.sun_path), "%s/socket", directory);
    CHECK(length > 0 && (size_t)length < sizeof(address.sun_path));
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + (size_t)length + 1u);
    maelys_sys_socket_t *listener = NULL, *client = NULL, *accepted = NULL;
    CHECK(maelys_sys_socket_create(AF_UNIX, SOCK_STREAM, 0, &listener) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_bind(listener, (struct sockaddr *)&address, address_length) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_listen(listener, 1) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_create(AF_UNIX, SOCK_STREAM, 0, &client) == MAELYS_SYS_OK);
    maelys_sys_connect_state_t state;
    CHECK(maelys_sys_socket_connect_start(client, (struct sockaddr *)&address, address_length, &state) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_accept(listener, NULL, NULL, &accepted) == MAELYS_SYS_OK);
    int client_fd = maelys_sys_socket_native_fd(client);
    int accepted_fd = maelys_sys_socket_native_fd(accepted);
#ifdef SO_PASSCRED
    if (passcred) {
        int enabled = 1;
        CHECK(setsockopt(client_fd, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
        CHECK(setsockopt(accepted_fd, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    }
#else
    (void)passcred;
#endif
    int source = open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    CHECK(exercise(accepted, client_fd, source) == 0);
    CHECK(exercise(client, accepted_fd, source) == 0);
    CHECK(close(source) == 0);
    CHECK(maelys_sys_socket_release(&client) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_release(&accepted) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_release(&listener) == MAELYS_SYS_OK);
    CHECK(unlink(address.sun_path) == 0);
    CHECK(rmdir(directory) == 0);
    return 0;
}

int main(void) {
    struct rlimit limit;
    CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    if (limit.rlim_cur > FD_LIMIT) limit.rlim_cur = FD_LIMIT;
    CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    (void)alarm(15);
    int baseline = open_count();
    CHECK(connected_pair(0) == 0);
#ifdef SO_PASSCRED
    CHECK(connected_pair(1) == 0);
#endif
    CHECK(open_count() == baseline);
    puts("socket rights: ok");
    return 0;
}
