#define _POSIX_C_SOURCE 200809L

#include "maelys/sys.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static int parse_iterations(const char *text, uint64_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !end || *end || value < 1u) return 0;
    *out = (uint64_t)value;
    return 1;
}

static int readiness(uint64_t iterations, uint64_t *elapsed) {
    maelys_sys_loop_t *loop = NULL;
    int sockets[2] = {-1, -1};
    maelys_sys_watch_t watch = 0;
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) != MAELYS_SYS_OK ||
        maelys_sys_socketpair_cloexec(SOCK_STREAM, sockets) != MAELYS_SYS_OK ||
        maelys_sys_loop_watch_fd(loop, sockets[0], MAELYS_SYS_INTEREST_READ,
            1, &watch) != MAELYS_SYS_OK) return 0;
    uint64_t start = nanoseconds();
    for (uint64_t i = 0; i < iterations; ++i) {
        if (write(sockets[1], "x", 1) != 1) return 0;
        maelys_sys_event_t event;
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        if (maelys_sys_loop_step(loop, MAELYS_SYS_DEADLINE_INFINITE,
                &event, 1, &count, &step) != MAELYS_SYS_OK ||
            count != 1 || event.token != 1) return 0;
        char byte = 0;
        if (read(sockets[0], &byte, 1) != 1) return 0;
    }
    *elapsed = nanoseconds() - start;
    (void)maelys_sys_loop_unwatch(loop, watch);
    (void)maelys_sys_fd_close(&sockets[0]);
    (void)maelys_sys_fd_close(&sockets[1]);
    (void)maelys_sys_loop_destroy(&loop);
    return 1;
}

static int timers(uint64_t iterations, uint64_t *elapsed) {
    maelys_sys_loop_t *loop = NULL;
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) != MAELYS_SYS_OK) return 0;
    uint64_t start = nanoseconds();
    for (uint64_t i = 0; i < iterations; ++i) {
        uint64_t now = 0;
        maelys_sys_timer_t timer = 0;
        if (maelys_sys_monotonic_ms(&now) != MAELYS_SYS_OK ||
            maelys_sys_loop_timer_add(loop, now, 1, &timer) != MAELYS_SYS_OK) return 0;
        maelys_sys_event_t event;
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        if (maelys_sys_loop_step(loop, now, &event, 1, &count, &step) !=
                MAELYS_SYS_OK || count != 1 || event.token != 1) return 0;
    }
    *elapsed = nanoseconds() - start;
    (void)maelys_sys_loop_destroy(&loop);
    return 1;
}

static void report(const char *workload, uint64_t iterations, uint64_t elapsed) {
    double per_operation = (double)elapsed / (double)iterations;
    printf("maelys-system,%s,%" PRIu64 ",%" PRIu64 ",%.2f\n",
        workload, iterations, elapsed, per_operation);
}

int main(int argc, char **argv) {
    uint64_t iterations = 10000;
    if (argc == 2 && !parse_iterations(argv[1], &iterations)) return 2;
    if (argc > 2) return 2;
    uint64_t elapsed = 0;
    if (!readiness(iterations, &elapsed)) return 1;
    report("readiness-roundtrip", iterations, elapsed);
    if (!timers(iterations, &elapsed)) return 1;
    report("immediate-one-shot-timer", iterations, elapsed);
    return 0;
}
