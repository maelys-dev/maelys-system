/*
 * Descriptor passing over AF_UNIX SOCK_DGRAM. This unit stands alone: it
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
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

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

maelys_sys_result_t maelys_sys_fd_send(
    int socket_fd,
    const void *bytes,
    size_t length,
    int passed_fd) {
    if ((!bytes && length) || passed_fd < -1) return MAELYS_SYS_ERR_ARGUMENT;
    maelys_sys_result_t checked = check_socket(socket_fd);
    if (checked != MAELYS_SYS_OK) return checked;
    if (passed_fd >= 0 && fcntl(passed_fd, F_GETFD) < 0) return MAELYS_SYS_ERR_ARGUMENT;

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
        sent = sendmsg(socket_fd, &message, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) return send_failure(errno);
    return MAELYS_SYS_OK;
}

/* Closes every descriptor a receive handed over, stored or not. */
static void close_received(const int *received, size_t count) {
    for (size_t index = 0; index < count; ++index) (void)close(received[index]);
}

maelys_sys_result_t maelys_sys_fd_receive(
    int socket_fd,
    void *buffer,
    size_t capacity,
    size_t *out_received,
    int *out_fds,
    size_t fd_capacity,
    size_t *out_fd_count,
    unsigned *out_flags) {
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
    maelys_sys_result_t checked = check_socket(socket_fd);
    if (checked != MAELYS_SYS_OK) return checked;

    /* macOS flags MSG_TRUNC on an empty datagram read into an empty vector,
     * where Linux does not: a zero capacity reads into one scratch byte, so
     * an empty datagram is never called truncated. */
    unsigned char scratch = 0;
    struct iovec iov;
    iov.iov_base = capacity ? buffer : &scratch;
    iov.iov_len = capacity ? capacity : 1u;
    union {
        struct cmsghdr header;
        unsigned char space[CMSG_SPACE(sizeof(int) * MAELYS_SYS_FDPASS_CONTROL_FDS)];
    } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.space;
    message.msg_controllen = sizeof(control.space);
    ssize_t received;
    do {
        received = recvmsg(socket_fd, &message, FDPASS_RECEIVE_FLAGS);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MAELYS_SYS_ERR_WOULD_BLOCK;
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
    const unsigned char *control_end =
        (const unsigned char *)message.msg_control + message.msg_controllen;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
            header->cmsg_len < CMSG_LEN(0)) {
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
