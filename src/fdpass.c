/*
 * Descriptor passing over AF_UNIX SOCK_DGRAM and SOCK_STREAM. This unit stands alone: it
 * includes its public header and the C library, and calls nothing else of
 * maelys-system, so a consumer can compile it from this path into an
 * archive that does not link the library (see maelys/sys/fdpass.h).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "maelys/sys/fdpass.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

/* Private entry used by socket.c. Keep the declaration here so this source
 * still compiles with only the public headers; the white-box test includes
 * fdpass_internal.h as well and checks that the declarations agree. */
maelys_sys_result_t maelys_sys_unix_receive_bytes(
    int socket_fd, void *buffer, size_t capacity, size_t *out_received);

#ifndef MSG_NOSIGNAL
#error "MSG_NOSIGNAL is required"
#endif

/*
 * The most descriptors one message may carry: Linux refuses more than 253
 * (SCM_MAX_FD) and merges several SCM_RIGHTS headers into one when received;
 * macOS refuses more than 254 and more than one header. A buffer this size
 * therefore always holds them all, measured on both hosts. A white-box test
 * shrinks it to reach the truncation paths.
 */
#ifndef MAELYS_SYS_FDPASS_CONTROL_FDS
#define MAELYS_SYS_FDPASS_CONTROL_FDS 254
#endif

/* Linux sets close-on-exec in the receive itself. The white-box test forces
 * the fcntl branch, so Linux exercises what macOS runs. */
#if defined(MSG_CMSG_CLOEXEC) && !defined(MAELYS_SYS_FDPASS_FORCE_FCNTL)
#define FDPASS_RECEIVE_FLAGS MSG_CMSG_CLOEXEC
#define FDPASS_NEEDS_FCNTL 0
#else
#define FDPASS_RECEIVE_FLAGS 0
#define FDPASS_NEEDS_FCNTL 1
#endif

/* Fault seam of the white-box test; production builds compile it away. */
#ifdef MAELYS_SYS_FDPASS_TESTING
static int fdpass_fault(const char *step);
#define FDPASS_FAULT(step) (fdpass_fault(step) != 0)
#else
#define FDPASS_FAULT(step) 0
#endif

/* A connected-or-not AF_UNIX SOCK_DGRAM socket, or an argument error. */
static maelys_sys_result_t check_socket(int socket_fd) {
    if (socket_fd < 0) return MAELYS_SYS_ERR_ARGUMENT;
    int type = 0;
    socklen_t type_length = (socklen_t)sizeof(type);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, &type, &type_length) != 0 ||
        type != SOCK_DGRAM) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    struct sockaddr_storage address;
    socklen_t address_length = (socklen_t)sizeof(address);
    memset(&address, 0, sizeof(address));
    if (getsockname(socket_fd, (struct sockaddr *)&address, &address_length) != 0 ||
        address.ss_family != AF_UNIX) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    return MAELYS_SYS_OK;
}

/* What sendmsg(2) said, as one code for what the hosts spell differently. */
static maelys_sys_result_t send_failure(int error) {
    switch (error) {
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case ENOBUFS:
            return MAELYS_SYS_ERR_WOULD_BLOCK;
        case ECONNREFUSED:
        case ECONNRESET:
        case EPIPE:
        case ENOTCONN:
        case EINVAL:
            return MAELYS_SYS_ERR_CLOSED;
        default:
            return MAELYS_SYS_ERR_OS;
    }
}

static ssize_t send_message(
    int socket_fd,
    const void *bytes,
    size_t length,
    int passed_fd, int resume_eintr) {
    struct iovec iov;
    iov.iov_base = (void *)(uintptr_t)bytes;
    iov.iov_len = length;
    union {
        struct cmsghdr header;
        unsigned char space[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (passed_fd >= 0) {
        message.msg_control = control.space;
        message.msg_controllen = CMSG_SPACE(sizeof(int));
        struct cmsghdr *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &passed_fd, sizeof(passed_fd));
    }
    ssize_t sent;
    do {
        sent = FDPASS_FAULT("sendmsg") ? -1 : sendmsg(socket_fd, &message, MSG_NOSIGNAL);
    } while (resume_eintr && sent < 0 && errno == EINTR);
    return sent;
}

maelys_sys_result_t maelys_sys_fd_send(
    int socket_fd, const void *bytes, size_t length, int passed_fd) {
    if ((!bytes && length) || passed_fd < -1) return MAELYS_SYS_ERR_ARGUMENT;
    maelys_sys_result_t checked = check_socket(socket_fd);
    if (checked != MAELYS_SYS_OK) return checked;
    if (passed_fd >= 0 && fcntl(passed_fd, F_GETFD) < 0) return MAELYS_SYS_ERR_ARGUMENT;
    ssize_t sent = send_message(socket_fd, bytes, length, passed_fd, 1);
    if (sent < 0) return send_failure(errno);
    return MAELYS_SYS_OK;
}

/* Closes every descriptor a receive handed over, stored or not. */
static void close_received(const int *received, size_t count) {
    for (size_t index = 0; index < count; ++index) (void)close(received[index]);
}

static maelys_sys_result_t receive_message(
    int socket_fd,
    void *buffer,
    size_t capacity,
    size_t *out_received,
    int *out_fds,
    size_t fd_capacity,
    size_t *out_fd_count,
    unsigned *out_flags,
    int stream,
    int resume_eintr,
    int *out_control) {
    *out_control = 0;
    if (out_received) *out_received = 0;
    if (out_fd_count) *out_fd_count = 0;
    if (out_flags) *out_flags = 0;
    if (out_fds) {
        for (size_t index = 0; index < fd_capacity; ++index) out_fds[index] = -1;
    }
    if (!out_received || !out_fd_count || !out_flags ||
        (!buffer && capacity) || (!out_fds && fd_capacity)) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    /* macOS flags MSG_TRUNC on an empty datagram read into an empty vector,
     * where Linux does not: a zero capacity reads into one scratch byte, so
     * an empty datagram is never called truncated. */
    unsigned char scratch = 0;
    struct iovec iov;
    iov.iov_base = capacity ? buffer : &scratch;
    iov.iov_len = capacity ? capacity : 1u;
    union {
        struct cmsghdr header;
        /* Stream receives also leave room for kernel ancillary data (in
         * particular Linux SO_PASSCRED alongside the maximum rights). */
        unsigned char space[CMSG_SPACE(sizeof(int) * MAELYS_SYS_FDPASS_CONTROL_FDS) + 256];
    } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.space;
    message.msg_controllen = (socklen_t)(stream ? sizeof(control.space) :
        CMSG_SPACE(sizeof(int) * MAELYS_SYS_FDPASS_CONTROL_FDS));
    ssize_t received;
    do {
        received = FDPASS_FAULT("recvmsg") ? -1 : recvmsg(socket_fd, &message, FDPASS_RECEIVE_FLAGS);
    } while (resume_eintr && received < 0 && errno == EINTR);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MAELYS_SYS_ERR_WOULD_BLOCK;
        if (stream && errno == ENOTCONN) return MAELYS_SYS_ERR_CLOSED;
        if (stream && errno == ECONNRESET) return MAELYS_SYS_ERR_RESET;
        return MAELYS_SYS_ERR_OS;
    }

    /* Collect every descriptor the kernel installed. macOS may announce in
     * cmsg_len more than it copied when it truncated, so a header is read
     * only as far as the control data actually returned. */
    /* Sized by the buffer, not by the count it was made for: alignment can
     * leave room for one more descriptor, which the kernel then fills. */
    int descriptors[sizeof(control.space) / sizeof(int)];
    const size_t descriptor_room = sizeof(descriptors) / sizeof(descriptors[0]);
    size_t count = 0;
    int unexpected_control = 0;
    const unsigned char *control_end =
        (const unsigned char *)message.msg_control + message.msg_controllen;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0)) {
            unexpected_control = 1;
            continue;
        }
        const unsigned char *data = CMSG_DATA(header);
        size_t announced = ((size_t)header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        size_t present = data < control_end ?
            (size_t)(control_end - data) / sizeof(int) : 0;
        size_t available = announced < present ? announced : present;
        for (size_t index = 0; index < available && count < descriptor_room; ++index) {
            memcpy(&descriptors[count++], data + index * sizeof(int), sizeof(int));
        }
    }

    /* Linux may attach credentials even to EOF with SO_PASSCRED enabled.
     * Only rights (including truncated rights) make a zero-byte receive
     * something other than EOF here, not the presence of any control. */
    *out_control = count != 0 || (message.msg_flags & MSG_CTRUNC) != 0;
    if (FDPASS_NEEDS_FCNTL) {
        for (size_t index = 0; index < count; ++index) {
            int flags = FDPASS_FAULT("fcntl") ? -1 : fcntl(descriptors[index], F_GETFD);
            if (flags < 0 ||
                fcntl(descriptors[index], F_SETFD, flags | FD_CLOEXEC) != 0) {
                int saved = errno;
                close_received(descriptors, count);
                errno = saved;
                return MAELYS_SYS_ERR_OS;
            }
        }
    }

    unsigned flags = 0;
    if (stream && unexpected_control) flags |= MAELYS_SYS_FDPASS_UNEXPECTED_CONTROL;
    if ((message.msg_flags & MSG_TRUNC) || (!capacity && received > 0)) {
        flags |= MAELYS_SYS_FDPASS_TRUNCATED;
    }
    if (message.msg_flags & MSG_CTRUNC) flags |= MAELYS_SYS_FDPASS_CONTROL_TRUNCATED;
    size_t kept = count < fd_capacity ? count : fd_capacity;
    for (size_t index = 0; index < kept; ++index) out_fds[index] = descriptors[index];
    if (count > kept) {
        close_received(descriptors + kept, count - kept);
        flags |= MAELYS_SYS_FDPASS_SURPLUS;
    }
    *out_received = capacity ? (size_t)received : 0u;
    *out_fd_count = kept;
    *out_flags = flags;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_fd_receive(
    int socket_fd, void *buffer, size_t capacity, size_t *out_received,
    int *out_fds, size_t fd_capacity, size_t *out_fd_count, unsigned *out_flags) {
    if (out_received) *out_received = 0;
    if (out_fd_count) *out_fd_count = 0;
    if (out_flags) *out_flags = 0;
    if (out_fds) {
        for (size_t i = 0; i < fd_capacity; ++i) out_fds[i] = -1;
    }
    maelys_sys_result_t checked = check_socket(socket_fd);
    if (checked != MAELYS_SYS_OK) return checked;
    int control = 0;
    return receive_message(socket_fd, buffer, capacity, out_received, out_fds,
                           fd_capacity, out_fd_count, out_flags, 0, 1, &control);
}

maelys_sys_result_t maelys_sys_unix_receive_bytes(
    int socket_fd, void *buffer, size_t capacity, size_t *out_received) {
    /* A control-only record is not EOF on macOS. Read again, but bound the
     * work so a peer continuously supplying such records cannot monopolise
     * an event-loop thread. This is an error, not a false EOF/WouldBlock. */
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        size_t fd_count = 0;
        unsigned flags = 0;
        int control = 0;
        maelys_sys_result_t result = receive_message(socket_fd, buffer, capacity,
            out_received, NULL, 0, &fd_count, &flags, 1, 1, &control);
        if (result != MAELYS_SYS_OK) return result;
        if (flags & MAELYS_SYS_FDPASS_CONTROL_TRUNCATED) {
            errno = EMSGSIZE;
            *out_received = 0;
            return MAELYS_SYS_ERR_OS;
        }
        if (*out_received) return MAELYS_SYS_OK;
        if (!control) return MAELYS_SYS_ERR_CLOSED;
    }
    errno = EPROTO;
    return MAELYS_SYS_ERR_OS;
}

static maelys_sys_result_t check_stream(int socket_fd) {
    if (socket_fd < 0) return MAELYS_SYS_ERR_ARGUMENT;
    int type = 0;
    socklen_t length = (socklen_t)sizeof(type);
    struct sockaddr_storage address;
    socklen_t address_length = (socklen_t)sizeof(address);
    int flags = fcntl(socket_fd, F_GETFL);
    if (flags < 0 || !(flags & O_NONBLOCK) ||
        getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, &type, &length) != 0 ||
        type != SOCK_STREAM ||
        getsockname(socket_fd, (struct sockaddr *)&address, &address_length) != 0 ||
        address.ss_family != AF_UNIX) return MAELYS_SYS_ERR_ARGUMENT;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_fd_stream_send(
    int socket_fd, const void *bytes, size_t length, int passed_fd, size_t *out_sent) {
    if (out_sent) *out_sent = 0;
    if (!out_sent || (!bytes && length) || length > (size_t)SSIZE_MAX ||
        passed_fd < -1 || (!length && passed_fd >= 0)) return MAELYS_SYS_ERR_ARGUMENT;
    maelys_sys_result_t checked = check_stream(socket_fd);
    if (checked != MAELYS_SYS_OK) return checked;
    if (passed_fd >= 0 && fcntl(passed_fd, F_GETFD) < 0) return MAELYS_SYS_ERR_ARGUMENT;
    if (!length) return MAELYS_SYS_OK;
    ssize_t sent = send_message(socket_fd, bytes, length, passed_fd, 0);
    if (sent > 0) {
        *out_sent = (size_t)sent;
        return MAELYS_SYS_OK;
    }
    if (sent == 0 || errno == EAGAIN || errno == EWOULDBLOCK) return MAELYS_SYS_ERR_WOULD_BLOCK;
#ifdef __APPLE__
    if (passed_fd >= 0 && errno == EMSGSIZE) return MAELYS_SYS_ERR_WOULD_BLOCK;
#endif
    if (errno == EPIPE || errno == ENOTCONN) return MAELYS_SYS_ERR_CLOSED;
    if (errno == ECONNRESET) return MAELYS_SYS_ERR_RESET;
    return MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_fd_stream_receive(
    int socket_fd, void *buffer, size_t capacity, size_t *out_received,
    int *out_fds, size_t fd_capacity, size_t *out_fd_count, unsigned *out_flags) {
    if (out_received) *out_received = 0;
    if (out_fd_count) *out_fd_count = 0;
    if (out_flags) *out_flags = 0;
    if (out_fds) for (size_t i = 0; i < fd_capacity; ++i) out_fds[i] = -1;
    if (!out_received || !out_fd_count || !out_flags || !buffer ||
        (!out_fds && fd_capacity) || !capacity || capacity > (size_t)SSIZE_MAX) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    maelys_sys_result_t checked = check_stream(socket_fd);
    if (checked != MAELYS_SYS_OK) return checked;
    int control = 0;
    maelys_sys_result_t result = receive_message(socket_fd, buffer, capacity,
        out_received, out_fds, fd_capacity, out_fd_count, out_flags, 1, 0, &control);
    if (result == MAELYS_SYS_OK && !*out_received && !control) {
        *out_flags = 0;
        return MAELYS_SYS_ERR_CLOSED;
    }
    return result;
}
