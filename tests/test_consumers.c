#include "maelys/sys.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int netd_relay_fixture(void) {
    maelys_sys_loop_t *loop = NULL;
    int client[2] = {-1, -1};
    int upstream[2] = {-1, -1};
    CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, client) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, upstream) == MAELYS_SYS_OK);
    maelys_sys_watch_t client_watch = 0;
    maelys_sys_watch_t upstream_watch = 0;
    CHECK(maelys_sys_loop_watch_fd(loop, client[1], MAELYS_SYS_INTEREST_READ,
        1, &client_watch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_watch_fd(loop, upstream[1], MAELYS_SYS_INTEREST_READ,
        2, &upstream_watch) == MAELYS_SYS_OK);

    static const char request[] = "CONNECT example.invalid:443";
    CHECK(write(client[0], request, sizeof(request)) == (ssize_t)sizeof(request));
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(1000, &deadline) == MAELYS_SYS_OK);
    int relayed = 0;
    while (!relayed) {
        maelys_sys_event_t events[4];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        CHECK(maelys_sys_loop_step(loop, deadline, events, 4, &count, &step) ==
            MAELYS_SYS_OK);
        CHECK(step != MAELYS_SYS_STEP_TIMEOUT);
        for (size_t i = 0; i < count; ++i) {
            if (events[i].token != 1 || !(events[i].flags & MAELYS_SYS_EVENT_READ)) {
                continue;
            }
            char buffer[64];
            ssize_t got = read(client[1], buffer, sizeof(buffer));
            CHECK(got == (ssize_t)sizeof(request));
            CHECK(write(upstream[1], buffer, (size_t)got) == got);
            relayed = 1;
        }
    }
    char observed[64] = {0};
    CHECK(read(upstream[0], observed, sizeof(observed)) == (ssize_t)sizeof(request));
    CHECK(memcmp(observed, request, sizeof(request)) == 0);
    CHECK(maelys_sys_loop_unwatch(loop, upstream_watch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_unwatch(loop, client_watch) == MAELYS_SYS_OK);
    for (size_t i = 0; i < 2; ++i) {
        CHECK(maelys_sys_fd_close(&client[i]) == MAELYS_SYS_OK);
        CHECK(maelys_sys_fd_close(&upstream[i]) == MAELYS_SYS_OK);
    }
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

typedef struct capture_context {
    maelys_sys_loop_t *loop;
    int write_fd;
} capture_context_t;

static void *capture_producer(void *opaque) {
    capture_context_t *context = opaque;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 5000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    static const char output[] = "agent output\n";
    if (write(context->write_fd, output, sizeof(output)) !=
        (ssize_t)sizeof(output)) {
        return (void *)(uintptr_t)1;
    }
    return (void *)(uintptr_t)maelys_sys_loop_wake(context->loop);
}

static int orchestrator_capture_fixture(void) {
    maelys_sys_loop_t *loop = NULL;
    int output[2] = {-1, -1};
    CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
    CHECK(maelys_sys_pipe_cloexec(output) == MAELYS_SYS_OK);
    maelys_sys_watch_t watch = 0;
    CHECK(maelys_sys_loop_watch_fd(loop, output[0], MAELYS_SYS_INTEREST_READ,
        7, &watch) == MAELYS_SYS_OK);
    capture_context_t context = {.loop = loop, .write_fd = output[1]};
    maelys_sys_thread_t *thread = NULL;
    CHECK(maelys_sys_thread_create(
        "capture", capture_producer, &context, &thread) == MAELYS_SYS_OK);
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(1000, &deadline) == MAELYS_SYS_OK);
    int captured = 0;
    while (!captured) {
        maelys_sys_event_t events[4];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        CHECK(maelys_sys_loop_step(loop, deadline, events, 4, &count, &step) ==
            MAELYS_SYS_OK);
        CHECK(step != MAELYS_SYS_STEP_TIMEOUT);
        for (size_t i = 0; i < count; ++i) {
            if (events[i].token == 7 && (events[i].flags & MAELYS_SYS_EVENT_READ)) {
                char buffer[32] = {0};
                CHECK(read(output[0], buffer, sizeof(buffer)) == 14);
                CHECK(strcmp(buffer, "agent output\n") == 0);
                captured = 1;
            }
        }
    }
    void *thread_result = NULL;
    CHECK(maelys_sys_thread_join(&thread, &thread_result) == MAELYS_SYS_OK);
    CHECK((uintptr_t)thread_result == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&output[0]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&output[1]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

int main(void) {
    if (netd_relay_fixture() != 0) return 1;
    if (orchestrator_capture_fixture() != 0) return 1;
    puts("ok - netd relay fixture");
    puts("ok - orchestrator capture fixture");
    return 0;
}
