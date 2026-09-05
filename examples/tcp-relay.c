#define _POSIX_C_SOURCE 200809L

#include "maelys/sys.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define BUFFER_SIZE 16384u

typedef struct relay_buffer {
    unsigned char bytes[BUFFER_SIZE];
    size_t offset;
    size_t length;
    int source_eof;
    int target_shutdown;
} relay_buffer_t;

static int parse_port(const char *text, uint16_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value == 0 || value > 65535u) return 0;
    *out = (uint16_t)value;
    return 1;
}

static int make_address(const char *host, uint16_t port,
    struct sockaddr_storage *storage, socklen_t *out_length) {
    memset(storage, 0, sizeof(*storage));
    struct sockaddr_in *v4 = (struct sockaddr_in *)(void *)storage;
    if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(port);
        *out_length = (socklen_t)sizeof(*v4);
        return AF_INET;
    }
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)(void *)storage;
    if (inet_pton(AF_INET6, host, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(port);
        *out_length = (socklen_t)sizeof(*v6);
        return AF_INET6;
    }
    return 0;
}

/* Setup phase only: block until one direction of a handle is ready. */
static int wait_ready(const maelys_sys_socket_t *socket_handle, short events) {
    struct pollfd descriptor = {
        .fd = maelys_sys_socket_native_fd(socket_handle),
        .events = events,
        .revents = 0
    };
    int status;
    do {
        status = poll(&descriptor, 1, -1);
    } while (status < 0 && errno == EINTR);
    return status == 1 ? 0 : -1;
}

static int update_watch(maelys_sys_loop_t *loop, maelys_sys_watch_t watch,
    relay_buffer_t *incoming, relay_buffer_t *outgoing) {
    unsigned interests = 0;
    if (!incoming->source_eof && incoming->length < BUFFER_SIZE) {
        interests |= MAELYS_SYS_INTEREST_READ;
    }
    if (outgoing->length) interests |= MAELYS_SYS_INTEREST_WRITE;
    if (!interests) interests = MAELYS_SYS_INTEREST_READ;
    return maelys_sys_loop_modify(loop, watch, interests) == MAELYS_SYS_OK ? 0 : -1;
}

static int would_block(maelys_sys_result_t result) {
    return result == MAELYS_SYS_ERR_WOULD_BLOCK;
}

static int receive_into(maelys_sys_socket_t *source, relay_buffer_t *buffer) {
    if (buffer->length == BUFFER_SIZE) return 0;
    size_t tail = (buffer->offset + buffer->length) % BUFFER_SIZE;
    size_t available = BUFFER_SIZE - buffer->length;
    size_t contiguous = BUFFER_SIZE - tail;
    if (contiguous > available) contiguous = available;
    size_t got = 0;
    maelys_sys_result_t result = maelys_sys_socket_receive(
        source, buffer->bytes + tail, contiguous, &got);
    if (result == MAELYS_SYS_OK) {
        buffer->length += got;
        return 0;
    }
    if (result == MAELYS_SYS_ERR_CLOSED) {
        buffer->source_eof = 1;
        return 0;
    }
    return would_block(result) ? 0 : -1;
}

static int send_from(maelys_sys_socket_t *target, relay_buffer_t *buffer) {
    if (!buffer->length) return 0;
    size_t contiguous = BUFFER_SIZE - buffer->offset;
    if (contiguous > buffer->length) contiguous = buffer->length;
    size_t sent = 0;
    maelys_sys_result_t result = maelys_sys_socket_send(
        target, buffer->bytes + buffer->offset, contiguous, &sent);
    if (result == MAELYS_SYS_OK) {
        buffer->offset = (buffer->offset + sent) % BUFFER_SIZE;
        buffer->length -= sent;
        return 0;
    }
    return would_block(result) ? 0 : -1;
}

static int finish_half_close(maelys_sys_socket_t *target, relay_buffer_t *buffer) {
    if (!buffer->source_eof || buffer->length || buffer->target_shutdown) return 0;
    if (maelys_sys_socket_shutdown(target, SHUT_WR) != MAELYS_SYS_OK) return -1;
    buffer->target_shutdown = 1;
    return 0;
}

static int relay(maelys_sys_socket_t *client, maelys_sys_socket_t *upstream) {
    maelys_sys_loop_t *loop = NULL;
    maelys_sys_watch_t client_watch = 0;
    maelys_sys_watch_t upstream_watch = 0;
    /* Handles are non-blocking by construction; the loop borrows their fds. */
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) != MAELYS_SYS_OK ||
        maelys_sys_loop_watch_fd(loop, maelys_sys_socket_native_fd(client),
            MAELYS_SYS_INTEREST_READ, 1, &client_watch) != MAELYS_SYS_OK ||
        maelys_sys_loop_watch_fd(loop, maelys_sys_socket_native_fd(upstream),
            MAELYS_SYS_INTEREST_READ, 2, &upstream_watch) != MAELYS_SYS_OK) return -1;
    relay_buffer_t to_upstream = {0};
    relay_buffer_t to_client = {0};
    while (!(to_upstream.target_shutdown && to_client.target_shutdown)) {
        maelys_sys_event_t events[4];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        if (maelys_sys_loop_step(loop, MAELYS_SYS_DEADLINE_INFINITE,
                events, 4, &count, &step) != MAELYS_SYS_OK) return -1;
        for (size_t i = 0; i < count; ++i) {
            if (events[i].token == 1) {
                if ((events[i].flags & MAELYS_SYS_EVENT_READ) &&
                    receive_into(client, &to_upstream) != 0) return -1;
                if ((events[i].flags & MAELYS_SYS_EVENT_WRITE) &&
                    send_from(client, &to_client) != 0) return -1;
            } else if (events[i].token == 2) {
                if ((events[i].flags & MAELYS_SYS_EVENT_READ) &&
                    receive_into(upstream, &to_client) != 0) return -1;
                if ((events[i].flags & MAELYS_SYS_EVENT_WRITE) &&
                    send_from(upstream, &to_upstream) != 0) return -1;
            }
        }
        if (finish_half_close(upstream, &to_upstream) != 0 ||
            finish_half_close(client, &to_client) != 0 ||
            update_watch(loop, client_watch, &to_upstream, &to_client) != 0 ||
            update_watch(loop, upstream_watch, &to_client, &to_upstream) != 0) return -1;
    }
    if (maelys_sys_loop_unwatch(loop, client_watch) != MAELYS_SYS_OK ||
        maelys_sys_loop_unwatch(loop, upstream_watch) != MAELYS_SYS_OK ||
        maelys_sys_loop_destroy(&loop) != MAELYS_SYS_OK) return -1;
    return 0;
}

static int connect_upstream(int family, const struct sockaddr *address,
    socklen_t address_length, maelys_sys_socket_t **out_upstream) {
    maelys_sys_connect_state_t state;
    if (maelys_sys_socket_create(family, SOCK_STREAM, 0, out_upstream) !=
        MAELYS_SYS_OK) return -1;
    if (maelys_sys_socket_connect_start(
            *out_upstream, address, address_length, &state) != MAELYS_SYS_OK) {
        return -1;
    }
    if (state == MAELYS_SYS_CONNECT_IN_PROGRESS &&
        wait_ready(*out_upstream, POLLOUT) != 0) return -1;
    return maelys_sys_socket_connect_complete(*out_upstream) == MAELYS_SYS_OK ?
        0 : -1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("usage: tcp-relay LISTEN_PORT UPSTREAM_IP UPSTREAM_PORT");
        return 0;
    }
    uint16_t listen_port = 0;
    uint16_t upstream_port = 0;
    if (argc != 4 || !parse_port(argv[1], &listen_port) ||
        !parse_port(argv[3], &upstream_port)) return 2;
    struct sockaddr_storage upstream_address;
    socklen_t upstream_length = 0;
    int family = make_address(argv[2], upstream_port,
        &upstream_address, &upstream_length);
    if (!family) {
        fprintf(stderr, "UPSTREAM_IP must be a numeric IPv4 or IPv6 address\n");
        return 2;
    }

    maelys_sys_socket_t *listener = NULL;
    maelys_sys_socket_t *client = NULL;
    maelys_sys_socket_t *upstream = NULL;
    int result = -1;
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(listen_port);
    maelys_sys_socket_bind_options_t bind_options = {0};
    bind_options.reuse_address = 1;
    if (inet_pton(AF_INET, "127.0.0.1", &local.sin_addr) == 1 &&
        maelys_sys_socket_create(AF_INET, SOCK_STREAM, 0, &listener) ==
            MAELYS_SYS_OK &&
        maelys_sys_socket_bind_with(listener, (struct sockaddr *)(void *)&local,
            (socklen_t)sizeof(local), &bind_options) == MAELYS_SYS_OK &&
        maelys_sys_socket_listen(listener, 1) == MAELYS_SYS_OK &&
        wait_ready(listener, POLLIN) == 0 &&
        maelys_sys_socket_accept(listener, NULL, NULL, &client) == MAELYS_SYS_OK &&
        connect_upstream(family, (struct sockaddr *)(void *)&upstream_address,
            upstream_length, &upstream) == 0) {
        result = relay(client, upstream);
    }
    (void)maelys_sys_socket_release(&upstream);
    (void)maelys_sys_socket_release(&client);
    (void)maelys_sys_socket_release(&listener);
    return result == 0 ? 0 : 1;
}
