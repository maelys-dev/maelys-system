#ifndef MAELYS_SYS_FDPASS_INTERNAL_H
#define MAELYS_SYS_FDPASS_INTERNAL_H

#include "maelys/sys/result.h"
#include <stddef.h>

/* Byte-only AF_UNIX receive. Discards every delivered descriptor using the
 * same control parser as fdpass. stream says what zero bytes mean: the end
 * of a stream, or one empty message on a datagram socket, which has no
 * end. Private: no new transport API in ABI 1. */
maelys_sys_result_t maelys_sys_unix_receive_bytes(
    int socket_fd, int stream, void *buffer, size_t capacity, size_t *out_received);

#endif
