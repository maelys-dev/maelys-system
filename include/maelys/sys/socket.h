#ifndef MAELYS_SYS_SOCKET_H
#define MAELYS_SYS_SOCKET_H

#include <stddef.h>
#include <sys/socket.h>

#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_sys_socket maelys_sys_socket_t;

typedef enum maelys_sys_connect_state {
    MAELYS_SYS_CONNECT_CONNECTED = 0,
    MAELYS_SYS_CONNECT_IN_PROGRESS
} maelys_sys_connect_state_t;

/*
 * Creates an owned POSIX socket which is non-blocking, close-on-exec and
 * protected against SIGPIPE before it is returned. No DNS, address selection
 * or retry policy is performed. The handle is owner-thread-confined.
 */
maelys_sys_result_t maelys_sys_socket_create(
    int domain,
    int type,
    int protocol,
    maelys_sys_socket_t **out_socket);

/*
 * Borrowed descriptor view. It remains valid until socket_release and must
 * never be closed by the caller. Returns -1 for NULL.
 */
int maelys_sys_socket_native_fd(const maelys_sys_socket_t *socket_handle);

/*
 * The address is borrowed only for the duration of the call. A start may be
 * performed exactly once per socket. IN_PROGRESS requires the caller to wait
 * for write/error readiness and then call connect_complete. On ERR_OS, errno
 * identifies connect(2) or SO_ERROR. No address retry is attempted.
 */
maelys_sys_result_t maelys_sys_socket_connect_start(
    maelys_sys_socket_t *socket_handle,
    const struct sockaddr *address,
    socklen_t address_length,
    maelys_sys_connect_state_t *out_state);
maelys_sys_result_t maelys_sys_socket_connect_complete(
    maelys_sys_socket_t *socket_handle);

/*
 * Non-blocking, partial socket I/O. On OK, the byte count is positive unless
 * the requested send length was zero. Would-block is ERR_OS with EAGAIN or
 * EWOULDBLOCK. Receive EOF and connection-loss conditions are ERR_CLOSED.
 * Send never raises SIGPIPE.
 */
maelys_sys_result_t maelys_sys_socket_receive(
    maelys_sys_socket_t *socket_handle,
    void *buffer,
    size_t capacity,
    size_t *out_received);
maelys_sys_result_t maelys_sys_socket_send(
    maelys_sys_socket_t *socket_handle,
    const void *bytes,
    size_t length,
    size_t *out_written);

/*
 * SHUT_RD, SHUT_WR and SHUT_RDWR are accepted. Repeating an already-applied
 * direction is an idempotent success. Shutting down an unconnected handle
 * makes a later connect_start invalid. This does not close or release the
 * handle.
 */
maelys_sys_result_t maelys_sys_socket_shutdown(
    maelys_sys_socket_t *socket_handle,
    int how);

/*
 * Mechanical server operations. Address storage follows the native POSIX
 * bind/accept lifetime rules. listen backlog must be non-negative. Accepted
 * handles have the same non-blocking, CLOEXEC and SIGPIPE guarantees as
 * socket_create. No listener policy or connection limit is implied.
 */
maelys_sys_result_t maelys_sys_socket_bind(
    maelys_sys_socket_t *socket_handle,
    const struct sockaddr *address,
    socklen_t address_length);
maelys_sys_result_t maelys_sys_socket_listen(
    maelys_sys_socket_t *socket_handle,
    int backlog);
maelys_sys_result_t maelys_sys_socket_accept(
    maelys_sys_socket_t *listener,
    struct sockaddr *address,
    socklen_t *address_length,
    maelys_sys_socket_t **out_socket);

/*
 * Closes exactly once, releases ownership and sets *socket_handle to NULL
 * before close(2). NULL and an already-NULL handle are idempotent successes.
 */
maelys_sys_result_t maelys_sys_socket_release(
    maelys_sys_socket_t **socket_handle);

#ifdef __cplusplus
}
#endif

#endif
