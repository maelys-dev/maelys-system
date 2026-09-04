#ifndef MAELYS_SYS_FD_H
#define MAELYS_SYS_FD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

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
 * SIGPIPE suppression is per-call when MSG_NOSIGNAL is available. On systems
 * that only provide SO_NOSIGPIPE, the function enables that persistent socket
 * option as a documented portability fallback.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_socket_send_nosigpipe(
    int fd,
    const void *bytes,
    size_t length,
    size_t *out_written);

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
