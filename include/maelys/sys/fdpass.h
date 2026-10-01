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
    MAELYS_SYS_FDPASS_SURPLUS = 1u << 2,
    /* Stream receive only: ancillary data other than SCM_RIGHTS arrived.
     * The caller may reject it; it is not silently accepted as a frame. */
    MAELYS_SYS_FDPASS_UNEXPECTED_CONTROL = 1u << 3
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
 * The surplus is closed first and never marked. Should marking a descriptor
 * fail, every descriptor of the message is closed, those already marked
 * included, and the call is ERR_OS: the datagram is consumed and lost.
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

/*
 * Partial I/O on a connected AF_UNIX SOCK_STREAM socket. Both operations
 * require O_NONBLOCK (checked, ERR_ARGUMENT otherwise), never change socket
 * flags, suppress SIGPIPE on send, and make at most one sendmsg/recvmsg call.
 * On ERR_OS/EINTR nothing transferred; the caller decides whether to retry
 * within its own deadline. No framing, offsets, clocks or retry policy live
 * here. The datagram operations above retain their original semantics.
 *
 * Send clears out_sent on entry. A positive out_sent means that prefix was
 * queued, together with passed_fd if supplied: retry the remaining bytes
 * WITHOUT the descriptor. This is not an acknowledgement by the receiver.
 * On an error out_sent is zero and no descriptor was sent; it may be attached
 * on a retry. The sender always keeps its original descriptor. passed_fd=-1
 * means none. A zero length is allowed only without a descriptor (OK, zero
 * progress); Linux drops and macOS delivers rights on an empty send, so that
 * combination is ERR_ARGUMENT. length must not exceed SSIZE_MAX.
 *
 * ERR_WOULD_BLOCK includes EAGAIN/EWOULDBLOCK and, when sending rights on
 * macOS, EMSGSIZE: insufficient room for the control record, including a
 * full queue. This does not promise a retry will succeed; socket buffer
 * limits can prevent a record from fitting even after data drains. Near-full
 * queues can instead return EAGAIN; using a blocking socket there can hang.
 * ERR_CLOSED is EPIPE/ENOTCONN; ERR_RESET is ECONNRESET. Other native errors
 * remain ERR_OS with errno, rather than being relabelled as temporary.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_fd_stream_send(
    int socket_fd, const void *bytes, size_t length, int passed_fd,
    size_t *out_sent);

/*
 * Receive reads no more than capacity (positive and <= SSIZE_MAX). It always
 * reads control; mixing read/recv/MSG_PEEK with these calls is unsafe. The
 * output clearing, ownership, surplus closing, CLOEXEC and fcntl-failure
 * rules of fd_receive apply. out_fd_count counts stored descriptors, not
 * those closed as surplus; fd_capacity may be zero. The control buffer
 * covers the per-message rights maximum plus Linux credentials. It does
 * not provide arbitrary ancillary-data transport. Truncation is flagged
 * and must be rejected by a protocol requiring a complete transfer.
 *
 * OK need not advance bytes: macOS can deliver zero bytes with rights. Check
 * out_fd_count AND SURPLUS even then; no rights are lost from observation.
 * Zero bytes without rights is ERR_CLOSED, including the synthetic
 * credentials Linux SO_PASSCRED may attach to EOF. Outputs are cleared on
 * errors. ERR_RESET reports native ECONNRESET (Linux may report it when a
 * peer closes with unread data, where macOS can report EOF instead). A
 * protocol must validate frame completeness even on ERR_CLOSED.
 *
 * An error after the read has still consumed it. Should marking a returned
 * descriptor close-on-exec fail, the call is ERR_OS with out_received 0 and
 * every descriptor closed, yet the bytes of that read are gone from the
 * stream: the caller no longer knows where its frame stands and must abandon
 * the exchange, not retry. CONTROL_TRUNCATED on OK calls for the same.
 *
 * Frame assembly, bounded work, descriptor cardinality, closing descriptors
 * held across partial reads, and abandonment on timeout are the caller's
 * responsibilities. Returned descriptors inherit their open-file-description
 * status flags; only close-on-exec is added.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_fd_stream_receive(
    int socket_fd, void *buffer, size_t capacity, size_t *out_received,
    int *out_fds, size_t fd_capacity, size_t *out_fd_count, unsigned *out_flags);

#ifdef __cplusplus
}
#endif

#endif
