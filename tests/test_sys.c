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
#include <time.h>
#include <unistd.h>

#define ASSERT_TRUE(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d assertion failed: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int test_version_and_results(void) {
    ASSERT_TRUE(strcmp(maelys_sys_version_string(), "0.5.4") == 0);
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
    int enabled = 1;
    char buffer[16] = {0};
    size_t transferred = 0u;

    ASSERT_TRUE(maelys_sys_socket_create(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_create(
        AF_INET, SOCK_STREAM, IPPROTO_TCP, &unconnected) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_shutdown(
        unconnected, SHUT_RDWR) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_shutdown(
        unconnected, SHUT_RDWR) == MAELYS_SYS_OK);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(1u);
    ASSERT_TRUE(maelys_sys_socket_connect_start(unconnected,
        (const struct sockaddr *)&address, (socklen_t)sizeof(address),
        &connect_state) == MAELYS_SYS_ERR_STATE);
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
    ASSERT_TRUE(setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
        &enabled, sizeof(enabled)) == 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    ASSERT_TRUE(maelys_sys_socket_bind(listener,
        (const struct sockaddr *)&address,
        (socklen_t)sizeof(address)) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_socket_listen(listener, 4) == MAELYS_SYS_OK);
    ASSERT_TRUE(getsockname(listener_fd, (struct sockaddr *)&address,
        &address_length) == 0);
    errno = 0;
    ASSERT_TRUE(maelys_sys_socket_accept(
        listener, NULL, NULL, &accepted) == MAELYS_SYS_ERR_OS);
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
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
    ASSERT_TRUE(result == MAELYS_SYS_ERR_CLOSED || result == MAELYS_SYS_ERR_OS);
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
        ASSERT_TRUE(result == MAELYS_SYS_ERR_OS);
        ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
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

    ASSERT_TRUE(maelys_sys_mutex_lock(context.mutex) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_condition_wait_until(context.condition, context.mutex,
        MAELYS_SYS_DEADLINE_INFINITE) == MAELYS_SYS_ERR_ARGUMENT);
    uint64_t deadline = 0;
    ASSERT_TRUE(maelys_sys_deadline_after(5, &deadline) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_condition_wait_until(
        context.condition, context.mutex, deadline) == MAELYS_SYS_ERR_TIMEOUT);
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
    ASSERT_TRUE(maelys_sys_loop_stop(loop) == MAELYS_SYS_OK);
    ASSERT_TRUE(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    ASSERT_TRUE(loop == NULL);
    ASSERT_TRUE(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
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
        test_socket_lifecycle,
        test_socket_send,
        test_socket_send_deadline,
        test_wakeup,
        test_threads_and_condition,
        test_loop_backends
    };
    const char *names[] = {
        "version and results",
        "clock and deadlines",
        "fd contracts",
        "socket lifecycle",
        "socket send",
        "socket send deadline",
        "wakeup",
        "threads and condition",
        "loop backends"
    };
    size_t count = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < count; ++i) {
        if (tests[i]() != 0) return 1;
        printf("ok %zu - %s\n", i + 1, names[i]);
    }
    printf("1..%zu\n", count);
    return 0;
}
