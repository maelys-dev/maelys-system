/* pthread_getname_np on Linux. */
#define _GNU_SOURCE

#include "maelys/sys.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d assertion failed: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#ifndef MAELYS_SYS_EXPECTED_VERSION
#error "build the tests through the Makefile: it passes the VERSION file"
#endif

static int test_version_and_results(void) {
    /* The VERSION file, the header macro and the function agree. */
    ASSERT_TRUE(strcmp(MAELYS_SYS_VERSION, MAELYS_SYS_EXPECTED_VERSION) == 0);
    ASSERT_TRUE(strcmp(maelys_sys_version_string(), MAELYS_SYS_VERSION) == 0);
    ASSERT_TRUE(maelys_sys_abi_version() == 1u);
    ASSERT_TRUE(strcmp(maelys_sys_result_string(MAELYS_SYS_OK), "ok") == 0);
    return 0;
}

static int wait_descriptor(int fd, short events) {
    struct pollfd descriptor = {.fd = fd, .events = events, .revents = 0};
    int status;
    do {
        status = poll(&descriptor, 1, 1000);
    } while (status < 0 && errno == EINTR);
    return status == 1;
}

static int test_socket_lifecycle(void) {
    maelys_sys_socket_t *listener = NULL;
    maelys_sys_socket_t *client = NULL;
    maelys_sys_socket_t *accepted = NULL;
    maelys_sys_socket_t *unconnected = NULL;
    struct sockaddr_in address;
    socklen_t address_length = (socklen_t)sizeof(address);
    maelys_sys_connect_state_t connect_state;
    int listener_fd;
    int option_value = 0;
    socklen_t option_length = (socklen_t)sizeof(option_value);
    maelys_sys_socket_bind_options_t bind_options = {0};
    char buffer[16] = {0};
    size_t transferred = 0u;

    ASSERT_TRUE(maelys_sys_socket_create(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_create(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, &unconnected) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_connect_complete(unconnected) ==
        MAELYS_SYS_ERR_STATE);
    ASSERT_TRUE(maelys_sys_socket_shutdown(
        unconnected, SHUT_RDWR) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_shutdown(
        unconnected, SHUT_RDWR) == MAELYS_SYS_OK);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(1u);
    connect_state = MAELYS_SYS_CONNECT_CONNECTED;
    ASSERT_TRUE(maelys_sys_socket_connect_start(unconnected,
        (const struct sockaddr *)&address, (socklen_t)sizeof(address),
        &connect_state) == MAELYS_SYS_ERR_STATE);
    /* *out_state is written on OK only. */
    ASSERT_TRUE(connect_state == MAELYS_SYS_CONNECT_CONNECTED);
    ASSERT_TRUE(maelys_sys_socket_release(&unconnected) == MAELYS_SYS_OK);
    listener_fd = maelys_sys_socket_native_fd(listener);
    ASSERT_TRUE(listener_fd >= 0);
    ASSERT_TRUE((fcntl(listener_fd, F_GETFD) & FD_CLOEXEC) != 0);
    ASSERT_TRUE((fcntl(listener_fd, F_GETFL) & O_NONBLOCK) != 0);
#if defined(SO_NOSIGPIPE)
    {
        int protected_value = 0;
        socklen_t protected_length = (socklen_t)sizeof(protected_value);
        ASSERT_TRUE(getsockopt(listener_fd, SOL_SOCKET, SO_NOSIGPIPE,
            &protected_value, &protected_length) == 0);
        ASSERT_TRUE(protected_value == 1);
    }
#endif
    ASSERT_TRUE(getsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
        &option_value, &option_length) == 0);
    ASSERT_TRUE(option_value == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    bind_options.reuse_address = 1;
    ASSERT_TRUE(maelys_sys_socket_bind_with(listener,
        (const struct sockaddr *)&address,
        (socklen_t)sizeof(address), &bind_options) == MAELYS_SYS_OK);
    option_value = 0;
    option_length = (socklen_t)sizeof(option_value);
    ASSERT_TRUE(getsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
        &option_value, &option_length) == 0);
    ASSERT_TRUE(option_value != 0);
    ASSERT_TRUE(maelys_sys_socket_listen(listener, 4) == MAELYS_SYS_OK);
    ASSERT_TRUE(getsockname(listener_fd, (struct sockaddr *)&address,
        &address_length) == 0);
    ASSERT_TRUE(maelys_sys_socket_accept(
        listener, NULL, NULL, &accepted) == MAELYS_SYS_ERR_WOULD_BLOCK);
    ASSERT_TRUE(accepted == NULL);

    ASSERT_TRUE(maelys_sys_socket_create(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, &client) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_connect_start(client,
        (const struct sockaddr *)&address, address_length,
        &connect_state) == MAELYS_SYS_OK);
    if (connect_state == MAELYS_SYS_CONNECT_IN_PROGRESS) {
        ASSERT_TRUE(wait_descriptor(
            maelys_sys_socket_native_fd(client), POLLOUT));
        ASSERT_TRUE(maelys_sys_socket_connect_complete(client) == MAELYS_SYS_OK);
    }
    ASSERT_TRUE(maelys_sys_socket_connect_complete(client) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_connect_start(client,
        (const struct sockaddr *)&address, address_length,
        &connect_state) == MAELYS_SYS_ERR_STATE);
    ASSERT_TRUE(wait_descriptor(listener_fd, POLLIN));
    ASSERT_TRUE(maelys_sys_socket_accept(
        listener, NULL, NULL, &accepted) == MAELYS_SYS_OK);
    ASSERT_TRUE(accepted != NULL);
    ASSERT_TRUE((fcntl(maelys_sys_socket_native_fd(accepted), F_GETFD) &
        FD_CLOEXEC) != 0);
    ASSERT_TRUE((fcntl(maelys_sys_socket_native_fd(accepted), F_GETFL) &
        O_NONBLOCK) != 0);

    ASSERT_TRUE(maelys_sys_socket_send(
        client, "hello", 5u, &transferred) == MAELYS_SYS_OK);
    ASSERT_TRUE(transferred == 5u);
    ASSERT_TRUE(wait_descriptor(
        maelys_sys_socket_native_fd(accepted), POLLIN));
    ASSERT_TRUE(maelys_sys_socket_receive(
        accepted, buffer, sizeof(buffer), &transferred) == MAELYS_SYS_OK);
    ASSERT_TRUE(transferred == 5u && memcmp(buffer, "hello", 5u) == 0);

    ASSERT_TRUE(maelys_sys_socket_shutdown(client, SHUT_WR) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_shutdown(client, SHUT_WR) == MAELYS_SYS_OK);
    ASSERT_TRUE(wait_descriptor(
        maelys_sys_socket_native_fd(accepted), POLLIN));
    ASSERT_TRUE(maelys_sys_socket_receive(
        accepted, buffer, sizeof(buffer), &transferred) == MAELYS_SYS_ERR_CLOSED);
    ASSERT_TRUE(maelys_sys_socket_shutdown(client, SHUT_RDWR) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_shutdown(client, SHUT_RDWR) == MAELYS_SYS_OK);

    ASSERT_TRUE(maelys_sys_socket_release(&accepted) == MAELYS_SYS_OK);
    ASSERT_TRUE(accepted == NULL);
    ASSERT_TRUE(maelys_sys_socket_release(&client) == MAELYS_SYS_OK);
    ASSERT_TRUE(client == NULL);
    ASSERT_TRUE(maelys_sys_socket_release(&listener) == MAELYS_SYS_OK);
    ASSERT_TRUE(listener == NULL);
    ASSERT_TRUE(maelys_sys_socket_release(&listener) == MAELYS_SYS_OK);
    return 0;
}

static int test_socket_connect_refused(void) {
    maelys_sys_socket_t *listener = NULL;
    maelys_sys_socket_t *client = NULL;
    struct sockaddr_in address;
    socklen_t address_length = (socklen_t)sizeof(address);
    maelys_sys_connect_state_t state;
    maelys_sys_result_t result;

    /* Reserve a loopback port, then close it so the connect is refused. */
    ASSERT_TRUE(maelys_sys_socket_create(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) == MAELYS_SYS_OK);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_TRUE(maelys_sys_socket_bind(listener,
        (const struct sockaddr *)&address,
        (socklen_t)sizeof(address)) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_listen(listener, 1) == MAELYS_SYS_OK);
    ASSERT_TRUE(getsockname(maelys_sys_socket_native_fd(listener),
        (struct sockaddr *)&address, &address_length) == 0);
    ASSERT_TRUE(maelys_sys_socket_release(&listener) == MAELYS_SYS_OK);

    ASSERT_TRUE(maelys_sys_socket_create(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, &client) == MAELYS_SYS_OK);
    errno = 0;
    result = maelys_sys_socket_connect_start(client,
        (const struct sockaddr *)&address, address_length, &state);
    if (result == MAELYS_SYS_OK) {
        ASSERT_TRUE(state == MAELYS_SYS_CONNECT_IN_PROGRESS);
        ASSERT_TRUE(wait_descriptor(
            maelys_sys_socket_native_fd(client), POLLOUT));
    } else {
        ASSERT_TRUE(result == MAELYS_SYS_ERR_OS && errno == ECONNREFUSED);
    }
    errno = 0;
    ASSERT_TRUE(maelys_sys_socket_connect_complete(client) == MAELYS_SYS_ERR_OS);
    ASSERT_TRUE(errno == ECONNREFUSED);
    /* The failure is remembered and the start was consumed. */
    errno = 0;
    ASSERT_TRUE(maelys_sys_socket_connect_complete(client) == MAELYS_SYS_ERR_OS);
    ASSERT_TRUE(errno == ECONNREFUSED);
    ASSERT_TRUE(maelys_sys_socket_connect_start(client,
        (const struct sockaddr *)&address, address_length, &state) ==
        MAELYS_SYS_ERR_STATE);
    /* A failed connection may be detached: the caller knows what it holds. */
    {
        int detached = -1;
        ASSERT_TRUE(maelys_sys_socket_detach(&client, &detached) == MAELYS_SYS_OK);
        ASSERT_TRUE(client == NULL && detached >= 0);
        ASSERT_TRUE(maelys_sys_fd_close(&detached) == MAELYS_SYS_OK);
    }
    return 0;
}

/*
 * detach hands the descriptor over: the handle is gone, the descriptor
 * stays open, non-blocking and close-on-exec, and the caller may make it
 * blocking and keep using it. A connection in progress is not detachable.
 */
static int test_socket_detach(void) {
    maelys_sys_socket_t *listener = NULL;
    maelys_sys_socket_t *client = NULL;
    maelys_sys_socket_t *accepted = NULL;
    struct sockaddr_in address;
    socklen_t address_length = (socklen_t)sizeof(address);
    maelys_sys_connect_state_t state;
    int detached = -1;
    int flags;
    size_t transferred = 0u;
    char buffer[4] = {0};

    ASSERT_TRUE(maelys_sys_socket_detach(NULL, &detached) == MAELYS_SYS_ERR_ARGUMENT);
    ASSERT_TRUE(detached == -1);
    ASSERT_TRUE(maelys_sys_socket_detach(&client, &detached) ==
        MAELYS_SYS_ERR_ARGUMENT);

    ASSERT_TRUE(maelys_sys_socket_create(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) == MAELYS_SYS_OK);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_TRUE(maelys_sys_socket_bind(listener,
        (const struct sockaddr *)&address,
        (socklen_t)sizeof(address)) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_listen(listener, 1) == MAELYS_SYS_OK);
    ASSERT_TRUE(getsockname(maelys_sys_socket_native_fd(listener),
        (struct sockaddr *)&address, &address_length) == 0);

    ASSERT_TRUE(maelys_sys_socket_create(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, &client) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_detach(client ? &client : NULL, NULL) ==
        MAELYS_SYS_ERR_ARGUMENT);
    ASSERT_TRUE(client != NULL);
    ASSERT_TRUE(maelys_sys_socket_connect_start(client,
        (const struct sockaddr *)&address, address_length, &state) ==
        MAELYS_SYS_OK);
    if (state == MAELYS_SYS_CONNECT_IN_PROGRESS) {
        ASSERT_TRUE(maelys_sys_socket_detach(&client, &detached) ==
            MAELYS_SYS_ERR_STATE);
        ASSERT_TRUE(client != NULL && detached == -1);
        ASSERT_TRUE(wait_descriptor(
            maelys_sys_socket_native_fd(client), POLLOUT));
        ASSERT_TRUE(maelys_sys_socket_connect_complete(client) == MAELYS_SYS_OK);
    }
    ASSERT_TRUE(maelys_sys_socket_detach(&client, &detached) == MAELYS_SYS_OK);
    ASSERT_TRUE(client == NULL && detached >= 0);
    ASSERT_TRUE(maelys_sys_socket_release(&client) == MAELYS_SYS_OK);

    flags = fcntl(detached, F_GETFD);
    ASSERT_TRUE(flags >= 0 && (flags & FD_CLOEXEC) != 0);
    flags = fcntl(detached, F_GETFL);
    ASSERT_TRUE(flags >= 0 && (flags & O_NONBLOCK) != 0);
    ASSERT_TRUE(maelys_sys_fd_set_blocking(detached) == MAELYS_SYS_OK);
    ASSERT_TRUE((fcntl(detached, F_GETFL) & O_NONBLOCK) == 0);
    ASSERT_TRUE(maelys_sys_socket_send_nosigpipe(
        detached, "x", 1u, &transferred) == MAELYS_SYS_OK);
    ASSERT_TRUE(transferred == 1u);

    ASSERT_TRUE(wait_descriptor(maelys_sys_socket_native_fd(listener), POLLIN));
    ASSERT_TRUE(maelys_sys_socket_accept(
        listener, NULL, NULL, &accepted) == MAELYS_SYS_OK);
    ASSERT_TRUE(wait_descriptor(maelys_sys_socket_native_fd(accepted), POLLIN));
    ASSERT_TRUE(maelys_sys_socket_receive(
        accepted, buffer, sizeof(buffer), &transferred) == MAELYS_SYS_OK);
    ASSERT_TRUE(transferred == 1u && buffer[0] == 'x');

    ASSERT_TRUE(maelys_sys_fd_close(&detached) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_release(&accepted) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_release(&listener) == MAELYS_SYS_OK);
    return 0;
}

/*
 * AF_UNIX with a zero backlog. Linux answers EAGAIN once the backlog is full
 * and nothing is started; macOS accepts every connection. Either way a
 * handle reported connected must be able to send.
 */
static int test_socket_unix_backlog(void) {
    maelys_sys_socket_t *listener = NULL;
    maelys_sys_socket_t *clients[8] = {0};
    struct sockaddr_un address;
    maelys_sys_connect_state_t state;
    size_t written = 0;
    int refused = 0;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path),
        "/tmp/maelys-sys-test-%ld.sock", (long)getpid());
    (void)unlink(address.sun_path);
    ASSERT_TRUE(maelys_sys_socket_create(
        AF_UNIX, SOCK_STREAM, 0, &listener) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_bind(listener,
        (const struct sockaddr *)&address,
        (socklen_t)sizeof(address)) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_listen(listener, 0) == MAELYS_SYS_OK);

    for (size_t i = 0; i < 8; ++i) {
        maelys_sys_result_t result;
        ASSERT_TRUE(maelys_sys_socket_create(
            AF_UNIX, SOCK_STREAM, 0, &clients[i]) == MAELYS_SYS_OK);
        errno = 0;
        result = maelys_sys_socket_connect_start(clients[i],
            (const struct sockaddr *)&address, (socklen_t)sizeof(address),
            &state);
        if (result == MAELYS_SYS_ERR_WOULD_BLOCK) {
            /* Nothing started: the handle is fresh, a new start is legal. */
            ASSERT_TRUE(maelys_sys_socket_connect_complete(clients[i]) ==
                MAELYS_SYS_ERR_STATE);
            ASSERT_TRUE(maelys_sys_socket_connect_start(clients[i],
                (const struct sockaddr *)&address, (socklen_t)sizeof(address),
                &state) != MAELYS_SYS_ERR_STATE);
            ++refused;
            continue;
        }
        ASSERT_TRUE(result == MAELYS_SYS_OK);
        if (state == MAELYS_SYS_CONNECT_IN_PROGRESS) {
            ASSERT_TRUE(wait_descriptor(
                maelys_sys_socket_native_fd(clients[i]), POLLOUT));
        }
        ASSERT_TRUE(maelys_sys_socket_connect_complete(clients[i]) ==
            MAELYS_SYS_OK);
        ASSERT_TRUE(maelys_sys_socket_send(
            clients[i], "x", 1u, &written) == MAELYS_SYS_OK);
        ASSERT_TRUE(written == 1u);
    }
#if defined(__linux__)
    ASSERT_TRUE(refused > 0);
#else
    (void)refused;
#endif
    for (size_t i = 0; i < 8; ++i) {
        ASSERT_TRUE(maelys_sys_socket_release(&clients[i]) == MAELYS_SYS_OK);
    }
    ASSERT_TRUE(maelys_sys_socket_release(&listener) == MAELYS_SYS_OK);
    ASSERT_TRUE(unlink(address.sun_path) == 0);
    return 0;
}

static int test_clock_and_deadlines(void) {
    uint64_t mono = 0;
    uint64_t wall = 0;
    ASSERT_TRUE(maelys_sys_monotonic_ms(&mono) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_wall_ms(&wall) == MAELYS_SYS_OK);
    ASSERT_TRUE(wall > mono);
    uint64_t deadline = 0;
    ASSERT_TRUE(maelys_sys_deadline_after(50, &deadline) == MAELYS_SYS_OK);
    uint64_t remaining = 0;
    ASSERT_TRUE(maelys_sys_deadline_remaining(deadline, &remaining) == MAELYS_SYS_OK);
    ASSERT_TRUE(remaining <= 50);
    ASSERT_TRUE(maelys_sys_deadline_after(UINT64_MAX, &deadline) ==
        MAELYS_SYS_ERR_ARGUMENT);
    errno = 0;
    ASSERT_TRUE(maelys_sys_deadline_after(UINT64_MAX - 1u, &deadline) ==
        MAELYS_SYS_ERR_OS);
    ASSERT_TRUE(errno == EOVERFLOW);
    ASSERT_TRUE(maelys_sys_deadline_remaining(
        MAELYS_SYS_DEADLINE_INFINITE, &remaining) == MAELYS_SYS_OK);
    ASSERT_TRUE(remaining == MAELYS_SYS_DEADLINE_INFINITE);
    int expired = 1;
    ASSERT_TRUE(maelys_sys_deadline_expired(
        MAELYS_SYS_DEADLINE_INFINITE, &expired) == MAELYS_SYS_OK);
    ASSERT_TRUE(!expired);
    return 0;
}

/* One descriptor, one deadline: the wait a consumer needs without a loop. */
static int test_fd_wait(void) {
    int sockets[2] = {-1, -1};
    unsigned flags = 0;
    uint64_t deadline = 0, before = 0, after = 0;
    ASSERT_TRUE(maelys_sys_socketpair_cloexec(SOCK_STREAM, sockets) == MAELYS_SYS_OK);
    /* Writable at once, even with an infinite deadline. */
    ASSERT_TRUE(maelys_sys_fd_wait(sockets[0], MAELYS_SYS_INTEREST_WRITE,
        MAELYS_SYS_DEADLINE_INFINITE, &flags) == MAELYS_SYS_OK);
    ASSERT_TRUE(flags & MAELYS_SYS_EVENT_WRITE);
    /* Nothing to read: the deadline passes, fully. */
    ASSERT_TRUE(maelys_sys_monotonic_ms(&before) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_deadline_after(60, &deadline) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_fd_wait(sockets[0], MAELYS_SYS_INTEREST_READ,
        deadline, &flags) == MAELYS_SYS_ERR_TIMEOUT);
    ASSERT_TRUE(maelys_sys_monotonic_ms(&after) == MAELYS_SYS_OK);
    ASSERT_TRUE(after - before >= 50u && flags == 0);
    /* A byte arrives: READ, and both interests report both. */
    ASSERT_TRUE(write(sockets[1], "x", 1) == 1);
    ASSERT_TRUE(maelys_sys_fd_wait(sockets[0],
        MAELYS_SYS_INTEREST_READ | MAELYS_SYS_INTEREST_WRITE, deadline, &flags) ==
        MAELYS_SYS_OK);
    ASSERT_TRUE((flags & (MAELYS_SYS_EVENT_READ | MAELYS_SYS_EVENT_WRITE)) ==
        (MAELYS_SYS_EVENT_READ | MAELYS_SYS_EVENT_WRITE));
    /* The peer closes: READ with the byte still to read, HUP with it. */
    ASSERT_TRUE(maelys_sys_fd_close(&sockets[1]) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_fd_wait(sockets[0], MAELYS_SYS_INTEREST_READ,
        MAELYS_SYS_DEADLINE_INFINITE, &flags) == MAELYS_SYS_OK);
    ASSERT_TRUE(flags & MAELYS_SYS_EVENT_READ);
    ASSERT_TRUE(maelys_sys_fd_wait(-1, MAELYS_SYS_INTEREST_READ, deadline, &flags) ==
        MAELYS_SYS_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_sys_fd_wait(sockets[0], 0u, deadline, &flags) ==
        MAELYS_SYS_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_sys_fd_wait(sockets[0], MAELYS_SYS_INTEREST_READ, deadline,
        NULL) == MAELYS_SYS_ERR_ARGUMENT);
    int closed = sockets[0];
    ASSERT_TRUE(maelys_sys_fd_close(&sockets[0]) == MAELYS_SYS_OK);
    errno = 0;
    ASSERT_TRUE(maelys_sys_fd_wait(closed, MAELYS_SYS_INTEREST_READ, deadline,
        &flags) == MAELYS_SYS_ERR_ARGUMENT);
    ASSERT_TRUE(errno == EBADF);
    return 0;
}

static int test_fd_contracts(void) {
    int pipe_fds[2] = {-1, -1};
    ASSERT_TRUE(maelys_sys_pipe_cloexec(pipe_fds) == MAELYS_SYS_OK);
    ASSERT_TRUE((fcntl(pipe_fds[0], F_GETFD) & FD_CLOEXEC) != 0);
    ASSERT_TRUE((fcntl(pipe_fds[1], F_GETFD) & FD_CLOEXEC) != 0);
    ASSERT_TRUE(maelys_sys_fd_set_nonblocking(pipe_fds[0]) == MAELYS_SYS_OK);
    ASSERT_TRUE((fcntl(pipe_fds[0], F_GETFL) & O_NONBLOCK) != 0);
    ASSERT_TRUE(maelys_sys_fd_set_blocking(pipe_fds[0]) == MAELYS_SYS_OK);
    ASSERT_TRUE((fcntl(pipe_fds[0], F_GETFL) & O_NONBLOCK) == 0);
    int original = pipe_fds[0];
    ASSERT_TRUE(maelys_sys_fd_close(&pipe_fds[0]) == MAELYS_SYS_OK);
    ASSERT_TRUE(pipe_fds[0] == -1);
    ASSERT_TRUE(fcntl(original, F_GETFD) < 0 && errno == EBADF);
    ASSERT_TRUE(maelys_sys_fd_close(&pipe_fds[0]) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_fd_close(&pipe_fds[1]) == MAELYS_SYS_OK);
    return 0;
}

static volatile sig_atomic_t sigpipe_count;

static void sigpipe_handler(int signal_number) {
    (void)signal_number;
    ++sigpipe_count;
}

static int test_socket_send(void) {
    int sockets[2] = {-1, -1};
    ASSERT_TRUE(maelys_sys_socketpair_cloexec(SOCK_STREAM, sockets) ==
        MAELYS_SYS_OK);
    uint64_t deadline = 0;
    ASSERT_TRUE(maelys_sys_deadline_after(1000, &deadline) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_send_all_until(
        sockets[0], "x", 1, deadline) == MAELYS_SYS_ERR_ARGUMENT);
    ASSERT_TRUE(maelys_sys_fd_set_nonblocking(sockets[0]) == MAELYS_SYS_OK);
#if defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    int nosigpipe_before = 0;
    socklen_t option_length = sizeof(nosigpipe_before);
    ASSERT_TRUE(getsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE,
        &nosigpipe_before, &option_length) == 0);
#endif
    ASSERT_TRUE(maelys_sys_socket_send_all_until(
        sockets[0], "hello", 5, deadline) == MAELYS_SYS_OK);
#if defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    int nosigpipe_after = 0;
    option_length = sizeof(nosigpipe_after);
    ASSERT_TRUE(getsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE,
        &nosigpipe_after, &option_length) == 0);
    ASSERT_TRUE(nosigpipe_after == nosigpipe_before);
#endif
    char buffer[8] = {0};
    ASSERT_TRUE(read(sockets[1], buffer, sizeof(buffer)) == 5);
    ASSERT_TRUE(memcmp(buffer, "hello", 5) == 0);

    struct sigaction action;
    struct sigaction previous;
    memset(&action, 0, sizeof(action));
    action.sa_handler = sigpipe_handler;
    sigemptyset(&action.sa_mask);
    ASSERT_TRUE(sigaction(SIGPIPE, &action, &previous) == 0);
    ASSERT_TRUE(maelys_sys_fd_close(&sockets[1]) == MAELYS_SYS_OK);
    size_t written = 0;
    maelys_sys_result_t result = maelys_sys_socket_send_nosigpipe(
        sockets[0], "x", 1, &written);
    /* EPIPE, on both hosts, and no signal. */
    ASSERT_TRUE(result == MAELYS_SYS_ERR_CLOSED);
    ASSERT_TRUE(sigpipe_count == 0);
    ASSERT_TRUE(sigaction(SIGPIPE, &previous, NULL) == 0);
    ASSERT_TRUE(maelys_sys_fd_close(&sockets[0]) == MAELYS_SYS_OK);
    return 0;
}

static int test_socket_send_deadline(void) {
    int sockets[2] = {-1, -1};
    ASSERT_TRUE(maelys_sys_socketpair_cloexec(SOCK_STREAM, sockets) ==
        MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_fd_set_nonblocking(sockets[0]) == MAELYS_SYS_OK);
    char block[4096] = {0};
    for (;;) {
        size_t written = 0;
        maelys_sys_result_t result = maelys_sys_socket_send_nosigpipe(
            sockets[0], block, sizeof(block), &written);
        if (result == MAELYS_SYS_OK) {
            ASSERT_TRUE(written > 0);
            continue;
        }
        ASSERT_TRUE(result == MAELYS_SYS_ERR_WOULD_BLOCK);
        break;
    }
    uint64_t deadline = 0;
    ASSERT_TRUE(maelys_sys_deadline_after(20, &deadline) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_send_all_until(
        sockets[0], "x", 1, deadline) == MAELYS_SYS_ERR_TIMEOUT);
    int expired = 0;
    ASSERT_TRUE(maelys_sys_deadline_expired(deadline, &expired) == MAELYS_SYS_OK);
    ASSERT_TRUE(expired);
    ASSERT_TRUE((fcntl(sockets[0], F_GETFL) & O_NONBLOCK) != 0);
    ASSERT_TRUE(maelys_sys_fd_close(&sockets[0]) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_fd_close(&sockets[1]) == MAELYS_SYS_OK);
    return 0;
}

typedef struct wake_context {
    maelys_sys_wakeup_t *wakeup;
    int count;
} wake_context_t;

static void *signal_many(void *argument) {
    wake_context_t *context = argument;
    for (int i = 0; i < context->count; ++i) {
        if (maelys_sys_wakeup_signal(context->wakeup) != MAELYS_SYS_OK) {
            return (void *)(uintptr_t)1;
        }
    }
    return NULL;
}

static int test_wakeup(void) {
    maelys_sys_wakeup_t *wakeup = NULL;
    ASSERT_TRUE(maelys_sys_wakeup_create(&wakeup) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_wakeup_fd(wakeup) >= 0);
    ASSERT_TRUE(maelys_sys_wakeup_signal(wakeup) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_wakeup_signal(wakeup) == MAELYS_SYS_OK);
    struct pollfd descriptor = {
        .fd = maelys_sys_wakeup_fd(wakeup), .events = POLLIN, .revents = 0
    };
    ASSERT_TRUE(poll(&descriptor, 1, 1000) == 1);
    ASSERT_TRUE(maelys_sys_wakeup_consume(wakeup) == MAELYS_SYS_OK);
    descriptor.revents = 0;
    ASSERT_TRUE(poll(&descriptor, 1, 0) == 0);

    wake_context_t context = {.wakeup = wakeup, .count = 1000};
    maelys_sys_thread_t *threads[4] = {0};
    for (size_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(maelys_sys_thread_create(
            "wake-test", signal_many, &context, &threads[i]) == MAELYS_SYS_OK);
    }
    for (size_t i = 0; i < 4; ++i) {
        void *result = NULL;
        ASSERT_TRUE(maelys_sys_thread_join(&threads[i], &result) == MAELYS_SYS_OK);
        ASSERT_TRUE(result == NULL);
    }
    descriptor.revents = 0;
    ASSERT_TRUE(poll(&descriptor, 1, 1000) == 1);
    ASSERT_TRUE(maelys_sys_wakeup_consume(wakeup) == MAELYS_SYS_OK);
    maelys_sys_wakeup_destroy(wakeup);
    return 0;
}

typedef struct condition_context {
    maelys_sys_mutex_t *mutex;
    maelys_sys_condition_t *condition;
    int ready;
} condition_context_t;

static void *condition_worker(void *argument) {
    condition_context_t *context = argument;
    if (maelys_sys_mutex_lock(context->mutex) != MAELYS_SYS_OK) {
        return (void *)(uintptr_t)1;
    }
    context->ready = 1;
    if (maelys_sys_condition_signal(context->condition) != MAELYS_SYS_OK) {
        return (void *)(uintptr_t)2;
    }
    if (maelys_sys_mutex_unlock(context->mutex) != MAELYS_SYS_OK) {
        return (void *)(uintptr_t)3;
    }
    return (void *)(uintptr_t)42;
}

static int test_threads_and_condition(void) {
    condition_context_t context = {0};
    ASSERT_TRUE(maelys_sys_mutex_create(&context.mutex) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_condition_create(&context.condition) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_mutex_lock(context.mutex) == MAELYS_SYS_OK);
    maelys_sys_thread_t *thread = NULL;
    ASSERT_TRUE(maelys_sys_thread_create(
        "condition-test", condition_worker, &context, &thread) == MAELYS_SYS_OK);
    while (!context.ready) {
        uint64_t deadline = 0;
        ASSERT_TRUE(maelys_sys_deadline_after(1000, &deadline) == MAELYS_SYS_OK);
        ASSERT_TRUE(maelys_sys_condition_wait_until(
            context.condition, context.mutex, deadline) == MAELYS_SYS_OK);
    }
    ASSERT_TRUE(maelys_sys_mutex_unlock(context.mutex) == MAELYS_SYS_OK);
    void *result = NULL;
    ASSERT_TRUE(maelys_sys_thread_join(&thread, &result) == MAELYS_SYS_OK);
    ASSERT_TRUE((uintptr_t)result == 42);

    /* Without a deadline: the worker's signal is the only way out. */
    context.ready = 0;
    ASSERT_TRUE(maelys_sys_mutex_lock(context.mutex) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_thread_create(
        "condition-wait", condition_worker, &context, &thread) == MAELYS_SYS_OK);
    while (!context.ready) {
        ASSERT_TRUE(maelys_sys_condition_wait(context.condition, context.mutex) ==
            MAELYS_SYS_OK);
    }
    ASSERT_TRUE(maelys_sys_mutex_unlock(context.mutex) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_thread_join(&thread, &result) == MAELYS_SYS_OK);
    ASSERT_TRUE((uintptr_t)result == 42);
    ASSERT_TRUE(maelys_sys_condition_wait(NULL, context.mutex) == MAELYS_SYS_ERR_ARGUMENT);

    ASSERT_TRUE(maelys_sys_mutex_lock(context.mutex) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_condition_wait_until(context.condition, context.mutex,
        MAELYS_SYS_DEADLINE_INFINITE) == MAELYS_SYS_ERR_ARGUMENT);
    uint64_t deadline = 0;
    ASSERT_TRUE(maelys_sys_deadline_after(5, &deadline) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_condition_wait_until(
        context.condition, context.mutex, deadline) == MAELYS_SYS_ERR_TIMEOUT);
    /* The deadline is on the monotonic clock: a timeout lasts what it says.
     * With the wrong clock this returns after a millisecond. */
    {
        uint64_t before = 0, after = 0;
        ASSERT_TRUE(maelys_sys_monotonic_ms(&before) == MAELYS_SYS_OK);
        ASSERT_TRUE(maelys_sys_deadline_after(120, &deadline) == MAELYS_SYS_OK);
        ASSERT_TRUE(maelys_sys_condition_wait_until(
            context.condition, context.mutex, deadline) == MAELYS_SYS_ERR_TIMEOUT);
        ASSERT_TRUE(maelys_sys_monotonic_ms(&after) == MAELYS_SYS_OK);
        ASSERT_TRUE(after - before >= 100u);
    }
    ASSERT_TRUE(maelys_sys_mutex_unlock(context.mutex) == MAELYS_SYS_OK);
    maelys_sys_condition_destroy(context.condition);
    maelys_sys_mutex_destroy(context.mutex);
    return 0;
}

static int loop_deadline(uint64_t delay_ms, uint64_t *out_deadline) {
    return maelys_sys_deadline_after(delay_ms, out_deadline) == MAELYS_SYS_OK;
}

static int step_until_event(
    maelys_sys_loop_t *loop,
    maelys_sys_token_t token,
    unsigned required_flags) {
    uint64_t deadline = 0;
    ASSERT_TRUE(loop_deadline(1000, &deadline));
    for (;;) {
        maelys_sys_event_t events[8];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        maelys_sys_result_t result = maelys_sys_loop_step(
            loop, deadline, events, 8, &count, &step);
        if (result != MAELYS_SYS_OK) {
            fprintf(stderr, "loop step failed: %s (errno=%d: %s)\n",
                maelys_sys_result_string(result), errno, strerror(errno));
            return 1;
        }
        if (step == MAELYS_SYS_STEP_TIMEOUT) return 1;
        for (size_t i = 0; i < count; ++i) {
            if (events[i].token == token &&
                (events[i].flags & required_flags) == required_flags) {
                return 0;
            }
        }
    }
}

typedef struct loop_signal_context {
    maelys_sys_loop_t *loop;
    int stop;
} loop_signal_context_t;

static void *loop_signal_worker(void *argument) {
    loop_signal_context_t *context = argument;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    maelys_sys_result_t result = context->stop
        ? maelys_sys_loop_stop(context->loop)
        : maelys_sys_loop_wake(context->loop);
    return (void *)(uintptr_t)result;
}

static void *loop_wrong_owner_worker(void *argument) {
    maelys_sys_loop_t *loop = argument;
    maelys_sys_event_t event;
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
    maelys_sys_result_t result = maelys_sys_loop_step(
        loop, 0, &event, 1, &count, &step);
    return (void *)(uintptr_t)result;
}

static int exercise_loop_backend(maelys_sys_loop_backend_t backend) {
    maelys_sys_loop_t *loop = NULL;
    ASSERT_TRUE(maelys_sys_loop_create(backend, &loop) == MAELYS_SYS_OK);
    ASSERT_TRUE(loop != NULL);
    ASSERT_TRUE(maelys_sys_loop_backend_name(loop) != NULL);

    int sockets[2] = {-1, -1};
    ASSERT_TRUE(maelys_sys_socketpair_cloexec(SOCK_STREAM, sockets) ==
        MAELYS_SYS_OK);
    maelys_sys_watch_t watch = 0;
    ASSERT_TRUE(maelys_sys_loop_watch_fd(loop, sockets[0],
        MAELYS_SYS_INTEREST_READ, 101, &watch) == MAELYS_SYS_OK);
    maelys_sys_watch_t duplicate = 0;
    ASSERT_TRUE(maelys_sys_loop_watch_fd(loop, sockets[0],
        MAELYS_SYS_INTEREST_READ, 999, &duplicate) == MAELYS_SYS_ERR_STATE);

    ASSERT_TRUE(write(sockets[1], "x", 1) == 1);
    ASSERT_TRUE(step_until_event(loop, 101, MAELYS_SYS_EVENT_READ) == 0);
    char byte = 0;
    ASSERT_TRUE(read(sockets[0], &byte, 1) == 1 && byte == 'x');

    ASSERT_TRUE(maelys_sys_loop_modify(
        loop, watch, MAELYS_SYS_INTEREST_WRITE) == MAELYS_SYS_OK);
    ASSERT_TRUE(step_until_event(loop, 101, MAELYS_SYS_EVENT_WRITE) == 0);
    ASSERT_TRUE(maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_ERR_NOT_FOUND);

    maelys_sys_watch_t replacement = 0;
    ASSERT_TRUE(maelys_sys_loop_watch_fd(loop, sockets[0],
        MAELYS_SYS_INTEREST_READ, 102, &replacement) == MAELYS_SYS_OK);
    ASSERT_TRUE(replacement != watch);
    ASSERT_TRUE(maelys_sys_loop_modify(
        loop, watch, MAELYS_SYS_INTEREST_READ) == MAELYS_SYS_ERR_NOT_FOUND);

    uint64_t now = 0;
    ASSERT_TRUE(maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK);
    maelys_sys_timer_t cancelled = 0;
    ASSERT_TRUE(maelys_sys_loop_timer_add(loop, now, 201, &cancelled) ==
        MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_loop_timer_cancel(loop, cancelled) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_loop_timer_cancel(loop, cancelled) ==
        MAELYS_SYS_ERR_NOT_FOUND);
    maelys_sys_timer_t timer = 0;
    ASSERT_TRUE(maelys_sys_loop_timer_add(loop, now, 202, &timer) ==
        MAELYS_SYS_OK);
    ASSERT_TRUE(timer != cancelled);
    /* A watch id is never a timer id, whatever the slot numbers. */
    ASSERT_TRUE(maelys_sys_loop_timer_cancel(loop, (maelys_sys_timer_t)replacement) ==
        MAELYS_SYS_ERR_NOT_FOUND);
    ASSERT_TRUE(maelys_sys_loop_unwatch(loop, (maelys_sys_watch_t)timer) ==
        MAELYS_SYS_ERR_NOT_FOUND);
    ASSERT_TRUE(maelys_sys_loop_modify(loop, (maelys_sys_watch_t)timer,
        MAELYS_SYS_INTEREST_READ) == MAELYS_SYS_ERR_NOT_FOUND);
    ASSERT_TRUE(step_until_event(loop, 202, MAELYS_SYS_EVENT_TIMER) == 0);

    uint64_t timer_base = 0;
    ASSERT_TRUE(maelys_sys_monotonic_ms(&timer_base) == MAELYS_SYS_OK);
    maelys_sys_timer_t late_timer = 0;
    maelys_sys_timer_t early_timer = 0;
    maelys_sys_timer_t middle_timer = 0;
    ASSERT_TRUE(maelys_sys_loop_timer_add(
        loop, timer_base + 100u, 203, &late_timer) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_loop_timer_add(
        loop, timer_base + 20u, 204, &early_timer) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_loop_timer_add(
        loop, timer_base + 60u, 205, &middle_timer) == MAELYS_SYS_OK);
    maelys_sys_event_t timer_event;
    size_t timer_count = 0;
    maelys_sys_step_result_t timer_step = MAELYS_SYS_STEP_TIMEOUT;
    ASSERT_TRUE(maelys_sys_loop_step(loop, timer_base + 1000u, &timer_event, 1,
        &timer_count, &timer_step) == MAELYS_SYS_OK);
    uint64_t timer_finished = 0;
    ASSERT_TRUE(maelys_sys_monotonic_ms(&timer_finished) == MAELYS_SYS_OK);
    ASSERT_TRUE(timer_step == MAELYS_SYS_STEP_PROGRESS && timer_count == 1);
    ASSERT_TRUE(timer_event.flags == MAELYS_SYS_EVENT_TIMER);
    ASSERT_TRUE(timer_event.token == 204);
    ASSERT_TRUE(timer_finished < timer_base + 500u);
    ASSERT_TRUE(maelys_sys_loop_timer_cancel(loop, early_timer) ==
        MAELYS_SYS_ERR_NOT_FOUND);
    ASSERT_TRUE(maelys_sys_loop_timer_cancel(loop, middle_timer) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_loop_timer_cancel(loop, late_timer) == MAELYS_SYS_OK);

    loop_signal_context_t signal_context = {.loop = loop, .stop = 0};
    maelys_sys_thread_t *thread = NULL;
    ASSERT_TRUE(maelys_sys_thread_create(
        "loop-wake", loop_signal_worker, &signal_context, &thread) ==
        MAELYS_SYS_OK);
    ASSERT_TRUE(step_until_event(loop, 0, MAELYS_SYS_EVENT_WAKE) == 0);
    void *thread_result = NULL;
    ASSERT_TRUE(maelys_sys_thread_join(&thread, &thread_result) == MAELYS_SYS_OK);
    ASSERT_TRUE((uintptr_t)thread_result == MAELYS_SYS_OK);

    ASSERT_TRUE(maelys_sys_thread_create(
        "loop-owner", loop_wrong_owner_worker, loop, &thread) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_thread_join(&thread, &thread_result) == MAELYS_SYS_OK);
    ASSERT_TRUE((uintptr_t)thread_result == MAELYS_SYS_ERR_STATE);

    ASSERT_TRUE(maelys_sys_loop_unwatch(loop, replacement) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_fd_close(&sockets[1]) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_fd_close(&sockets[0]) == MAELYS_SYS_OK);

    signal_context.stop = 1;
    ASSERT_TRUE(maelys_sys_thread_create(
        "loop-stop", loop_signal_worker, &signal_context, &thread) ==
        MAELYS_SYS_OK);
    maelys_sys_event_t event;
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
    uint64_t deadline = 0;
    ASSERT_TRUE(loop_deadline(1000, &deadline));
    ASSERT_TRUE(maelys_sys_loop_step(
        loop, deadline, &event, 1, &count, &step) == MAELYS_SYS_OK);
    ASSERT_TRUE(step == MAELYS_SYS_STEP_STOPPED && count == 0);
    ASSERT_TRUE(maelys_sys_thread_join(&thread, &thread_result) == MAELYS_SYS_OK);
    ASSERT_TRUE((uintptr_t)thread_result == MAELYS_SYS_OK);
    /* stop is sticky: every later step reports it, without waiting. */
    for (int again = 0; again < 3; ++again) {
        ASSERT_TRUE(loop_deadline(1000, &deadline));
        ASSERT_TRUE(maelys_sys_loop_step(
            loop, deadline, &event, 1, &count, &step) == MAELYS_SYS_OK);
        ASSERT_TRUE(step == MAELYS_SYS_STEP_STOPPED && count == 0);
    }
    ASSERT_TRUE(maelys_sys_loop_stop(loop) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    ASSERT_TRUE(loop == NULL);
    ASSERT_TRUE(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

static volatile sig_atomic_t alarms;

static void alarm_handler(int signal_number) {
    (void)signal_number;
    ++alarms;
}

/* A signal every few milliseconds during a step: the wait is resumed with
 * the remaining time, not reported as an error nor cut short. */
static int test_loop_step_eintr(void) {
    maelys_sys_loop_t *loop = NULL;
    ASSERT_TRUE(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
    struct sigaction action;
    struct sigaction previous;
    memset(&action, 0, sizeof(action));
    action.sa_handler = alarm_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; /* no SA_RESTART: the wait returns EINTR */
    ASSERT_TRUE(sigaction(SIGALRM, &action, &previous) == 0);
    struct itimerval interval = {
        .it_interval = {.tv_sec = 0, .tv_usec = 5000},
        .it_value = {.tv_sec = 0, .tv_usec = 5000}
    };
    ASSERT_TRUE(setitimer(ITIMER_REAL, &interval, NULL) == 0);
    uint64_t before = 0, after = 0, deadline = 0;
    ASSERT_TRUE(maelys_sys_monotonic_ms(&before) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_deadline_after(100, &deadline) == MAELYS_SYS_OK);
    maelys_sys_event_t event;
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_PROGRESS;
    maelys_sys_result_t result = maelys_sys_loop_step(loop, deadline, &event, 1, &count, &step);
    ASSERT_TRUE(maelys_sys_monotonic_ms(&after) == MAELYS_SYS_OK);
    struct itimerval off = {{0, 0}, {0, 0}};
    ASSERT_TRUE(setitimer(ITIMER_REAL, &off, NULL) == 0);
    /* A signal TSan deferred may still be delivered: ignore it rather than
     * hand it to the default action, which would end the process. */
    action.sa_handler = SIG_IGN;
    ASSERT_TRUE(sigaction(SIGALRM, &action, NULL) == 0);
    (void)previous;
    ASSERT_TRUE(result == MAELYS_SYS_OK);
    ASSERT_TRUE(step == MAELYS_SYS_STEP_TIMEOUT && count == 0);
    /* Under TSan handlers run late and fewer; one interruption is enough. */
    ASSERT_TRUE(alarms >= 1);
    ASSERT_TRUE(after - before >= 90u);
    ASSERT_TRUE(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

static char observed_thread_name[64];

static void *name_reader(void *argument) {
    (void)argument;
    (void)pthread_getname_np(pthread_self(), observed_thread_name,
        sizeof(observed_thread_name));
    return NULL;
}

/* A name longer than the host limit is truncated, not dropped. */
static int test_thread_name_limit(void) {
    static const char long_name[] = "maelys-system-thread-name-longer-than-limits";
    maelys_sys_thread_t *thread = NULL;
    void *result = NULL;
    memset(observed_thread_name, 0, sizeof(observed_thread_name));
    ASSERT_TRUE(maelys_sys_thread_create(long_name, name_reader, NULL, &thread) ==
        MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_thread_join(&thread, &result) == MAELYS_SYS_OK);
#if defined(__linux__)
    ASSERT_TRUE(strlen(observed_thread_name) == 15u);
#else
    ASSERT_TRUE(strlen(observed_thread_name) == strlen(long_name));
#endif
    ASSERT_TRUE(strncmp(observed_thread_name, long_name, strlen(observed_thread_name)) == 0);
    return 0;
}

/*
 * connect_complete before readiness: with no peer yet, ERR_STATE. This needs
 * a connection that stays in progress, which only an unrouted address
 * gives; where the network refuses it outright the case is skipped.
 */
static int test_socket_connect_early_complete(void) {
    maelys_sys_socket_t *client = NULL;
    struct sockaddr_in address;
    maelys_sys_connect_state_t state;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0x0AFFFF01u); /* 10.255.255.1, unrouted */
    address.sin_port = htons(9);
    ASSERT_TRUE(maelys_sys_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &client) ==
        MAELYS_SYS_OK);
    maelys_sys_result_t started = maelys_sys_socket_connect_start(client,
        (const struct sockaddr *)&address, (socklen_t)sizeof(address), &state);
    if (started == MAELYS_SYS_OK && state == MAELYS_SYS_CONNECT_IN_PROGRESS) {
        errno = 0;
        ASSERT_TRUE(maelys_sys_socket_connect_complete(client) == MAELYS_SYS_ERR_STATE);
        ASSERT_TRUE(errno == ENOTCONN);
    } else {
        printf("# connect early complete skipped: no route to a silent address\n");
    }
    ASSERT_TRUE(maelys_sys_socket_release(&client) == MAELYS_SYS_OK);
    return 0;
}

static int test_loop_backends(void) {
    ASSERT_TRUE(maelys_sys_loop_backend_available(MAELYS_SYS_LOOP_AUTO));
    ASSERT_TRUE(maelys_sys_loop_backend_available(MAELYS_SYS_LOOP_POLL));
    ASSERT_TRUE(exercise_loop_backend(MAELYS_SYS_LOOP_POLL) == 0);
#ifdef __APPLE__
    ASSERT_TRUE(maelys_sys_loop_backend_available(MAELYS_SYS_LOOP_KQUEUE));
    ASSERT_TRUE(!maelys_sys_loop_backend_available(MAELYS_SYS_LOOP_EPOLL));
    ASSERT_TRUE(exercise_loop_backend(MAELYS_SYS_LOOP_KQUEUE) == 0);
#elif defined(__linux__)
    ASSERT_TRUE(maelys_sys_loop_backend_available(MAELYS_SYS_LOOP_EPOLL));
    ASSERT_TRUE(!maelys_sys_loop_backend_available(MAELYS_SYS_LOOP_KQUEUE));
    ASSERT_TRUE(exercise_loop_backend(MAELYS_SYS_LOOP_EPOLL) == 0);
#endif
    ASSERT_TRUE(exercise_loop_backend(MAELYS_SYS_LOOP_AUTO) == 0);
    return 0;
}

int main(void) {
    int (*tests[])(void) = {
        test_version_and_results,
        test_clock_and_deadlines,
        test_fd_contracts,
        test_fd_wait,
        test_socket_lifecycle,
        test_socket_connect_refused,
        test_socket_detach,
        test_socket_unix_backlog,
        test_socket_send,
        test_socket_send_deadline,
        test_wakeup,
        test_threads_and_condition,
        test_thread_name_limit,
        test_socket_connect_early_complete,
        test_loop_backends,
        test_loop_step_eintr
    };
    const char *names[] = {
        "version and results",
        "clock and deadlines",
        "fd contracts",
        "fd wait",
        "socket lifecycle",
        "socket connect refused",
        "socket detach",
        "socket unix backlog",
        "socket send",
        "socket send deadline",
        "wakeup",
        "threads and condition",
        "thread name limit",
        "socket connect early complete",
        "loop backends",
        "loop step under signals"
    };
    size_t count = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < count; ++i) {
        if (tests[i]() != 0) return 1;
        printf("ok %zu - %s\n", i + 1, names[i]);
    }
    printf("1..%zu\n", count);
    return 0;
}
