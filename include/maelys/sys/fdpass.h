#ifndef MAELYS_SYS_FDPASS_H
#define MAELYS_SYS_FDPASS_H

/*
 * Passing a descriptor over a connected AF_UNIX SOCK_DGRAM socket, with
 * SCM_RIGHTS. The gesture only: what the datagram means, and when a
 * descriptor is expected, belong to the caller's protocol.
 *
 * This unit stands alone, and that is part of the contract within ABI 1:
 * src/fdpass.c names no other symbol of the library, uses neither threads
 * nor clocks, and this header includes result.h for the enumeration only.
 * The object links by itself, without -pthread, on Linux and macOS, so a
 * consumer that must not link the library -- a client linked into a
 * confined process -- compiles src/fdpass.c from its pinned checkout into
 * its own archive, or extracts the member fdpass.o from an installed
 * libmaelys_sys.a, where no source is shipped. That member is exactly one,
 * keeps its name and is never merged into another object: a binary that
 * links such an archive and libmaelys_sys.a of the same pin then sees no
 * duplicate. Its path is stable. The build links a test to this object alone and refuses any
 * undefined symbol of the library or of the thread runtime in it.
 */

#include <stddef.h>

#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What maelys_sys_fd_receive reports beside its result. */
typedef enum maelys_sys_fdpass_flags {
    /* The datagram was longer than the buffer, or than zero when capacity
     * is 0; the rest is discarded. An empty datagram is never truncated. */
    MAELYS_SYS_FDPASS_TRUNCATED = 1u << 0,
    /* The kernel truncated the control data. The receive buffer holds every
     * descriptor either host lets one message carry, so this comes only from
     * other control data the caller enabled on the socket; no descriptor
     * leaks on either host. */
    MAELYS_SYS_FDPASS_CONTROL_TRUNCATED = 1u << 1,
    /* More descriptors arrived than fd_capacity: the surplus was closed here. */
    MAELYS_SYS_FDPASS_SURPLUS = 1u << 2
} maelys_sys_fdpass_flags_t;

/*
 * Sends one datagram and, unless passed_fd is -1, one descriptor with it.
 * socket_fd must be a connected AF_UNIX SOCK_DGRAM socket, checked here
 * (ERR_ARGUMENT otherwise, as for a passed_fd that is not open or below
 * -1). bytes may be NULL when length is 0: an empty datagram is a message.
 * The caller keeps passed_fd: the receiver gets a copy. SIGPIPE is
 * suppressed with MSG_NOSIGNAL and EINTR resumes the call.
 *
 * ERR_CLOSED when the peer is gone or reads no more, one code for what the
 * hosts spell differently: ECONNREFUSED on Linux and ECONNRESET on macOS
 * for a closed peer, EPIPE on Linux and EINVAL on macOS for a peer that shut
 * down reading, EPIPE for a local shutdown. This call builds the message
 * itself after checking its arguments, so EINVAL cannot mean a malformed
 * call here. ERR_WOULD_BLOCK when the peer's queue is full: Linux says
 * EAGAIN on a non-blocking socket and waits on a blocking one, macOS says
 * ENOBUFS in both modes and never waits, so on macOS this code comes back
 * even from a blocking socket. On ERR_OS errno identifies sendmsg(2).
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_fd_send(
    int socket_fd,
    const void *bytes,
    size_t length,
    int passed_fd);

/*
 * Receives one datagram into buffer and the descriptors that came with it
 * into out_fds, in arrival order; the caller owns and closes each one.
 * socket_fd must be a connected AF_UNIX SOCK_DGRAM socket, checked here.
 * The blocking mode is the socket's own; EINTR resumes the call. buffer may
 * be NULL when capacity is 0, out_fds when fd_capacity is 0.
 *
 * On entry the outputs are cleared: counts and flags to 0, every slot of
 * out_fds to -1. On OK *out_received holds the bytes copied, 0 for an empty
 * datagram, and *out_fd_count the descriptors stored. The control buffer
 * is sized for the most descriptors either host lets a message carry (253
 * on Linux, 254 on macOS, one header), so the kernel never truncates them:
 * a too-small buffer would let macOS install descriptors the caller cannot
 * see. Those beyond fd_capacity are closed here and SURPLUS is set, so a
 * protocol expecting one descriptor learns that more came.
 *
 * Each descriptor returned is close-on-exec: atomically with MSG_CMSG_CLOEXEC
 * on Linux; on macOS, which lacks it, with fcntl(2) right after recvmsg(2),
 * and a fork+exec racing in another thread can inherit one in between.
 * Should that fcntl fail, every descriptor received is closed and the call
 * is ERR_OS: the datagram is consumed and lost.
 *
 * A datagram socket has no end of stream: a peer that left shows on send,
 * not here, where the queue simply stays empty. ERR_WOULD_BLOCK on a
 * non-blocking socket with nothing queued; on ERR_OS errno identifies the
 * failed call.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_fd_receive(
    int socket_fd,
    void *buffer,
    size_t capacity,
    size_t *out_received,
    int *out_fds,
    size_t fd_capacity,
    size_t *out_fd_count,
    unsigned *out_flags);

#ifdef __cplusplus
}
#endif

#endif
