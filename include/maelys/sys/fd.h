#ifndef MAELYS_SYS_FD_H
#define MAELYS_SYS_FD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "maelys/sys/loop.h"
#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Close-on-exec is set atomically with creation on Linux (pipe2, SOCK_CLOEXEC,
 * accept4). macOS has no such calls: the flag is applied right after
 * creation, and a fork+exec racing in another thread can inherit the
 * descriptor. A consumer that launches processes closes descriptors after
 * fork rather than relying on the flag alone.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_fd_set_cloexec(int fd);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_fd_set_nonblocking(int fd);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_fd_set_blocking(int fd);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_pipe_cloexec(int out_fds[2]);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_socketpair_cloexec(
    int type,
    int out_fds[2]);

/*
 * Sets *fd to -1 before close(2). The call is never retried after EINTR: a
 * retry can close an unrelated descriptor if the number was already reused.
 * NULL and an already-negative descriptor are idempotent successes.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_fd_close(int *fd);

/*
 * Socket-only I/O. On success, *out_written may be shorter than length.
 * SIGPIPE is suppressed per call with MSG_NOSIGNAL, which both hosts
 * provide; no socket option is touched. ERR_WOULD_BLOCK when nothing can be
 * sent now, ERR_RESET when the peer reset the connection, ERR_CLOSED when
 * it is gone (EPIPE, ENOTCONN); on ERR_OS errno identifies send(2).
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_socket_send_nosigpipe(
    int fd,
    const void *bytes,
    size_t length,
    size_t *out_written);

/*
 * Waits until fd is ready for one of the interests (READ, WRITE) or the
 * absolute monotonic deadline passes; INFINITE is accepted. On OK
 * *out_flags carries what poll(2) reported, in the flags loop.h defines,
 * with HUP and ERROR as loop.h says, and is never empty. ERR_TIMEOUT when
 * the deadline passed, after one poll without wait so a ready descriptor is
 * reported even then; EINTR resumes the wait with the remaining time. A
 * closed descriptor is ERR_ARGUMENT with errno EBADF. One poll(2), for a
 * consumer that waits on a single descriptor and needs no loop.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_fd_wait(
    int fd,
    unsigned interests,
    uint64_t deadline_ms,
    unsigned *out_flags);

/*
 * Sends the complete buffer before the absolute monotonic deadline. A finite
 * deadline or MAELYS_SYS_DEADLINE_INFINITE is accepted. For a non-empty
 * buffer, fd must be a socket in non-blocking mode and must remain so for the
 * duration of the call; blocking sockets are rejected with ERR_ARGUMENT.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_socket_send_all_until(
    int fd,
    const void *bytes,
    size_t length,
    uint64_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif
