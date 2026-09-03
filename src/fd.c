#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "maelys/sys/clock.h"
#include "maelys/sys/fd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static maelys_sys_result_t set_flag(int fd, int get_command, int flag, int enabled) {
    if (fd < 0) return MAELYS_SYS_ERR_ARGUMENT;
    int current = fcntl(fd, get_command);
    if (current < 0) return MAELYS_SYS_ERR_OS;
    int updated = enabled ? current | flag : current & ~flag;
    int set_command = get_command == F_GETFD ? F_SETFD : F_SETFL;
    if (fcntl(fd, set_command, updated) != 0) return MAELYS_SYS_ERR_OS;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_fd_set_cloexec(int fd) {
    return set_flag(fd, F_GETFD, FD_CLOEXEC, 1);
}

maelys_sys_result_t maelys_sys_fd_set_nonblocking(int fd) {
    return set_flag(fd, F_GETFL, O_NONBLOCK, 1);
}

maelys_sys_result_t maelys_sys_fd_set_blocking(int fd) {
    return set_flag(fd, F_GETFL, O_NONBLOCK, 0);
}

maelys_sys_result_t maelys_sys_fd_close(int *fd) {
    if (!fd || *fd < 0) return MAELYS_SYS_OK;
    int closing = *fd;
    *fd = -1;
    return close(closing) == 0 ? MAELYS_SYS_OK : MAELYS_SYS_ERR_OS;
}

static maelys_sys_result_t finish_pair(int out_fds[2]) {
    maelys_sys_result_t first = maelys_sys_fd_set_cloexec(out_fds[0]);
    maelys_sys_result_t second = maelys_sys_fd_set_cloexec(out_fds[1]);
    if (first == MAELYS_SYS_OK && second == MAELYS_SYS_OK) return MAELYS_SYS_OK;
    int saved = errno;
    (void)maelys_sys_fd_close(&out_fds[0]);
    (void)maelys_sys_fd_close(&out_fds[1]);
    errno = saved;
    return MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_pipe_cloexec(int out_fds[2]) {
    if (!out_fds) return MAELYS_SYS_ERR_ARGUMENT;
    out_fds[0] = -1;
    out_fds[1] = -1;
#if defined(__linux__) && defined(O_CLOEXEC)
    if (pipe2(out_fds, O_CLOEXEC) == 0) return MAELYS_SYS_OK;
    if (errno != ENOSYS && errno != EINVAL) return MAELYS_SYS_ERR_OS;
#endif
    if (pipe(out_fds) != 0) return MAELYS_SYS_ERR_OS;
    return finish_pair(out_fds);
}

maelys_sys_result_t maelys_sys_socketpair_cloexec(int type, int out_fds[2]) {
    if (!out_fds) return MAELYS_SYS_ERR_ARGUMENT;
    out_fds[0] = -1;
    out_fds[1] = -1;
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    if (socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, out_fds) == 0) {
        return MAELYS_SYS_OK;
    }
    if (errno != EINVAL && errno != EPROTONOSUPPORT) return MAELYS_SYS_ERR_OS;
#endif
    if (socketpair(AF_UNIX, type, 0, out_fds) != 0) return MAELYS_SYS_ERR_OS;
    return finish_pair(out_fds);
}

maelys_sys_result_t maelys_sys_socket_send_nosigpipe(
    int fd,
    const void *bytes,
    size_t length,
    size_t *out_written) {
    if (fd < 0 || (!bytes && length) || !out_written) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    *out_written = 0;
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    int enabled = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return MAELYS_SYS_ERR_OS;
    }
#endif
    ssize_t written;
    do {
#ifdef MSG_NOSIGNAL
        written = send(fd, bytes, length, MSG_NOSIGNAL);
#else
        written = send(fd, bytes, length, 0);
#endif
    } while (written < 0 && errno == EINTR);
    if (written < 0) {
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
            return MAELYS_SYS_ERR_CLOSED;
        }
        return MAELYS_SYS_ERR_OS;
    }
    *out_written = (size_t)written;
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t wait_writable(int fd, uint64_t deadline_ms) {
    for (;;) {
        uint64_t remaining = 0;
        maelys_sys_result_t result =
            maelys_sys_deadline_remaining(deadline_ms, &remaining);
        if (result != MAELYS_SYS_OK) return result;
        if (remaining == 0) return MAELYS_SYS_ERR_TIMEOUT;
        int timeout = remaining == MAELYS_SYS_DEADLINE_INFINITE ? -1 :
            remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
        struct pollfd descriptor = {.fd = fd, .events = POLLOUT, .revents = 0};
        int ready = poll(&descriptor, 1, timeout);
        if (ready > 0) {
            if (descriptor.revents & POLLOUT) return MAELYS_SYS_OK;
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return MAELYS_SYS_ERR_CLOSED;
            }
            continue;
        }
        if (ready == 0) return MAELYS_SYS_ERR_TIMEOUT;
        if (errno != EINTR) return MAELYS_SYS_ERR_OS;
    }
}

maelys_sys_result_t maelys_sys_socket_send_all_until(
    int fd,
    const void *bytes,
    size_t length,
    uint64_t deadline_ms) {
    if (fd < 0 || (!bytes && length)) return MAELYS_SYS_ERR_ARGUMENT;
    if (length == 0) return MAELYS_SYS_OK;
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return MAELYS_SYS_ERR_OS;
    if ((flags & O_NONBLOCK) == 0) return MAELYS_SYS_ERR_ARGUMENT;
    const unsigned char *cursor = bytes;
    while (length > 0) {
        maelys_sys_result_t ready = wait_writable(fd, deadline_ms);
        if (ready != MAELYS_SYS_OK) return ready;
        size_t written = 0;
        maelys_sys_result_t sent =
            maelys_sys_socket_send_nosigpipe(fd, cursor, length, &written);
        if (sent == MAELYS_SYS_ERR_OS &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        if (sent != MAELYS_SYS_OK) return sent;
        if (written == 0) return MAELYS_SYS_ERR_CLOSED;
        cursor += written;
        length -= written;
    }
    return MAELYS_SYS_OK;
}
