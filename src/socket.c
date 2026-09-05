#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "maelys/sys/socket.h"

#include "maelys/sys/fd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct maelys_sys_socket {
    int fd;
    int connect_started;
    int connect_complete;
    int connect_error;
    unsigned shutdown_directions;
};

enum {
    SHUTDOWN_READ = 1u,
    SHUTDOWN_WRITE = 2u
};

static maelys_sys_result_t protect_socket(int fd) {
    maelys_sys_result_t result;
    result = maelys_sys_fd_set_cloexec(fd);
    if (result != MAELYS_SYS_OK) return result;
    result = maelys_sys_fd_set_nonblocking(fd);
    if (result != MAELYS_SYS_OK) return result;
#if defined(SO_NOSIGPIPE)
    {
        int enabled = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                       &enabled, sizeof(enabled)) != 0) {
            return MAELYS_SYS_ERR_OS;
        }
    }
#endif
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t wrap_socket(
    int fd,
    int already_nonblocking_cloexec,
    maelys_sys_socket_t **out_socket) {
    maelys_sys_socket_t *socket_handle;
    maelys_sys_result_t result;
    if (out_socket) *out_socket = NULL;
    if (fd < 0 || !out_socket) return MAELYS_SYS_ERR_ARGUMENT;
    if (!already_nonblocking_cloexec) {
        result = protect_socket(fd);
    } else {
#if defined(SO_NOSIGPIPE)
        int enabled = 1;
        result = setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                            &enabled, sizeof(enabled)) == 0 ?
            MAELYS_SYS_OK : MAELYS_SYS_ERR_OS;
#else
        result = MAELYS_SYS_OK;
#endif
    }
    if (result != MAELYS_SYS_OK) return result;
    socket_handle = calloc(1u, sizeof(*socket_handle));
    if (!socket_handle) return MAELYS_SYS_ERR_MEMORY;
    socket_handle->fd = fd;
    *out_socket = socket_handle;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_socket_create(
    int domain,
    int type,
    int protocol,
    maelys_sys_socket_t **out_socket) {
    int fd = -1;
    int atomic_flags = 0;
    int saved;
    maelys_sys_result_t result;
    if (out_socket) *out_socket = NULL;
    if (!out_socket || type < 0) return MAELYS_SYS_ERR_ARGUMENT;
#if defined(__linux__) && defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    atomic_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    fd = socket(domain, type | atomic_flags, protocol);
    if (fd < 0 && errno != EINVAL && errno != EPROTONOSUPPORT) {
        return MAELYS_SYS_ERR_OS;
    }
#endif
    if (fd < 0) {
        atomic_flags = 0;
        fd = socket(domain, type, protocol);
        if (fd < 0) return MAELYS_SYS_ERR_OS;
    }
    result = wrap_socket(fd, atomic_flags != 0, out_socket);
    if (result == MAELYS_SYS_OK) return result;
    saved = errno;
    (void)close(fd);
    errno = saved;
    return result;
}

int maelys_sys_socket_native_fd(const maelys_sys_socket_t *socket_handle) {
    return socket_handle ? socket_handle->fd : -1;
}

maelys_sys_result_t maelys_sys_socket_connect_start(
    maelys_sys_socket_t *socket_handle,
    const struct sockaddr *address,
    socklen_t address_length,
    maelys_sys_connect_state_t *out_state) {
    int status;
    if (!socket_handle || socket_handle->fd < 0 || !address ||
        address_length == 0 || !out_state) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (socket_handle->connect_started || socket_handle->shutdown_directions) {
        return MAELYS_SYS_ERR_STATE;
    }
    do {
        status = connect(socket_handle->fd, address, address_length);
    } while (status != 0 && errno == EINTR);
    if (status == 0 || errno == EISCONN) {
        socket_handle->connect_started = 1;
        socket_handle->connect_complete = 1;
        *out_state = MAELYS_SYS_CONNECT_CONNECTED;
        return MAELYS_SYS_OK;
    }
    if (errno == EINPROGRESS || errno == EALREADY) {
        socket_handle->connect_started = 1;
        *out_state = MAELYS_SYS_CONNECT_IN_PROGRESS;
        return MAELYS_SYS_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Nothing was started (Linux AF_UNIX with a full backlog): there is
         * no completion to wait for, and the handle stays fresh so the
         * caller may start again later. */
        return MAELYS_SYS_ERR_OS;
    }
    socket_handle->connect_started = 1;
    socket_handle->connect_error = errno;
    return MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_socket_connect_complete(
    maelys_sys_socket_t *socket_handle) {
    int error = 0;
    socklen_t length = (socklen_t)sizeof(error);
    if (!socket_handle || socket_handle->fd < 0) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (!socket_handle->connect_started) return MAELYS_SYS_ERR_STATE;
    if (socket_handle->connect_complete) return MAELYS_SYS_OK;
    if (socket_handle->connect_error) {
        errno = socket_handle->connect_error;
        return MAELYS_SYS_ERR_OS;
    }
    if (getsockopt(socket_handle->fd, SOL_SOCKET, SO_ERROR,
                   &error, &length) != 0) {
        return MAELYS_SYS_ERR_OS;
    }
    if (error != 0) {
        socket_handle->connect_error = error;
        errno = error;
        return MAELYS_SYS_ERR_OS;
    }
    {
        /* A clear SO_ERROR is not a connection: confirm a peer exists so a
         * completion attempted before readiness is reported as such. */
        struct sockaddr_storage peer;
        socklen_t peer_length = (socklen_t)sizeof(peer);
        if (getpeername(socket_handle->fd, (struct sockaddr *)(void *)&peer,
                        &peer_length) != 0) {
            return errno == ENOTCONN ? MAELYS_SYS_ERR_STATE : MAELYS_SYS_ERR_OS;
        }
    }
    socket_handle->connect_complete = 1;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_socket_receive(
    maelys_sys_socket_t *socket_handle,
    void *buffer,
    size_t capacity,
    size_t *out_received) {
    ssize_t received;
    if (out_received) *out_received = 0u;
    if (!socket_handle || socket_handle->fd < 0 || !buffer || !capacity ||
        !out_received) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    do {
        received = recv(socket_handle->fd, buffer, capacity, 0);
    } while (received < 0 && errno == EINTR);
    if (received > 0) {
        *out_received = (size_t)received;
        return MAELYS_SYS_OK;
    }
    if (received == 0 || errno == ECONNRESET || errno == ENOTCONN) {
        return MAELYS_SYS_ERR_CLOSED;
    }
    return MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_socket_send(
    maelys_sys_socket_t *socket_handle,
    const void *bytes,
    size_t length,
    size_t *out_written) {
    if (!socket_handle || socket_handle->fd < 0) {
        if (out_written) *out_written = 0u;
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    return maelys_sys_socket_send_nosigpipe(
        socket_handle->fd, bytes, length, out_written);
}

maelys_sys_result_t maelys_sys_socket_shutdown(
    maelys_sys_socket_t *socket_handle,
    int how) {
    unsigned requested;
    unsigned missing;
    int effective;
    if (!socket_handle || socket_handle->fd < 0) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (how == SHUT_RD) requested = SHUTDOWN_READ;
    else if (how == SHUT_WR) requested = SHUTDOWN_WRITE;
    else if (how == SHUT_RDWR) requested = SHUTDOWN_READ | SHUTDOWN_WRITE;
    else return MAELYS_SYS_ERR_ARGUMENT;
    missing = requested & ~socket_handle->shutdown_directions;
    if (!missing) return MAELYS_SYS_OK;
    effective = missing == SHUTDOWN_READ ? SHUT_RD :
        missing == SHUTDOWN_WRITE ? SHUT_WR : SHUT_RDWR;
    if (shutdown(socket_handle->fd, effective) != 0) {
        if (errno == ENOTCONN || errno == ECONNRESET) {
            socket_handle->shutdown_directions |= missing;
            return MAELYS_SYS_OK;
        }
        return MAELYS_SYS_ERR_OS;
    }
    socket_handle->shutdown_directions |= missing;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_socket_bind_with(
    maelys_sys_socket_t *socket_handle,
    const struct sockaddr *address,
    socklen_t address_length,
    const maelys_sys_socket_bind_options_t *options) {
    if (!socket_handle || socket_handle->fd < 0 || !address ||
        address_length == 0) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (options && options->reuse_address) {
        int enabled = 1;
        if (setsockopt(socket_handle->fd, SOL_SOCKET, SO_REUSEADDR,
                       &enabled, sizeof(enabled)) != 0) {
            return MAELYS_SYS_ERR_OS;
        }
    }
    return bind(socket_handle->fd, address, address_length) == 0 ?
        MAELYS_SYS_OK : MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_socket_bind(
    maelys_sys_socket_t *socket_handle,
    const struct sockaddr *address,
    socklen_t address_length) {
    return maelys_sys_socket_bind_with(
        socket_handle, address, address_length, NULL);
}

maelys_sys_result_t maelys_sys_socket_listen(
    maelys_sys_socket_t *socket_handle,
    int backlog) {
    if (!socket_handle || socket_handle->fd < 0 || backlog < 0) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    return listen(socket_handle->fd, backlog) == 0 ?
        MAELYS_SYS_OK : MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_socket_accept(
    maelys_sys_socket_t *listener,
    struct sockaddr *address,
    socklen_t *address_length,
    maelys_sys_socket_t **out_socket) {
    int fd = -1;
    int atomic_flags = 0;
    int saved;
    maelys_sys_result_t result;
    if (out_socket) *out_socket = NULL;
    if (!listener || listener->fd < 0 || !out_socket ||
        ((address == NULL) != (address_length == NULL))) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
#if defined(__linux__) && defined(SOCK_NONBLOCK) && defined(SOCK_CLOEXEC)
    do {
        fd = accept4(listener->fd, address, address_length,
                     SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0) atomic_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
    else if (errno != ENOSYS && errno != EINVAL) return MAELYS_SYS_ERR_OS;
#endif
    if (fd < 0) {
        do {
            fd = accept(listener->fd, address, address_length);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0) return MAELYS_SYS_ERR_OS;
    }
    result = wrap_socket(fd, atomic_flags != 0, out_socket);
    if (result == MAELYS_SYS_OK) return result;
    saved = errno;
    (void)close(fd);
    errno = saved;
    return result;
}

maelys_sys_result_t maelys_sys_socket_detach(
    maelys_sys_socket_t **socket_handle,
    int *out_fd) {
    maelys_sys_socket_t *owned;
    if (out_fd) *out_fd = -1;
    if (!socket_handle || !*socket_handle || !out_fd) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    owned = *socket_handle;
    if (owned->connect_started && !owned->connect_complete &&
        !owned->connect_error) {
        return MAELYS_SYS_ERR_STATE;
    }
    *socket_handle = NULL;
    *out_fd = owned->fd;
    memset(owned, 0, sizeof(*owned));
    free(owned);
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_socket_release(
    maelys_sys_socket_t **socket_handle) {
    maelys_sys_socket_t *owned;
    maelys_sys_result_t result;
    if (!socket_handle || !*socket_handle) return MAELYS_SYS_OK;
    owned = *socket_handle;
    *socket_handle = NULL;
    result = maelys_sys_fd_close(&owned->fd);
    memset(owned, 0, sizeof(*owned));
    free(owned);
    return result;
}
