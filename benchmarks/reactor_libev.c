#define _POSIX_C_SOURCE 200809L

#include <ev.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int ready;

static void mark_io(EV_P_ ev_io *watcher, int events) {
    (void)loop;
    (void)watcher;
    (void)events;
    ready = 1;
}

static void mark_timer(EV_P_ ev_timer *watcher, int events) {
    (void)events;
    ready = 1;
    ev_timer_stop(EV_A_ watcher);
}

static uint64_t nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static uint64_t iterations_from(int argc, char **argv) {
    if (argc != 2) return 10000;
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(argv[1], &end, 10);
    return errno || !end || *end || value < 1u ? 0 : (uint64_t)value;
}

static void report(const char *workload, uint64_t count, uint64_t elapsed) {
    printf("libev,%s,%" PRIu64 ",%" PRIu64 ",%.2f\n",
        workload, count, elapsed, (double)elapsed / (double)count);
}

int main(int argc, char **argv) {
    uint64_t iterations = iterations_from(argc, argv);
    if (!iterations) return 2;
    struct ev_loop *loop = ev_loop_new(0);
    int sockets[2];
    if (!loop || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return 1;
    ev_io io;
    ev_io_init(&io, mark_io, sockets[0], EV_READ);
    ev_io_start(loop, &io);
    uint64_t start = nanoseconds();
    for (uint64_t i = 0; i < iterations; ++i) {
        ready = 0;
        if (write(sockets[1], "x", 1) != 1) return 1;
        while (!ready) ev_run(loop, EVRUN_ONCE);
        char byte;
        if (read(sockets[0], &byte, 1) != 1) return 1;
    }
    report("readiness-roundtrip", iterations, nanoseconds() - start);
    ev_timer timer;
    ev_timer_init(&timer, mark_timer, 0.0, 0.0);
    start = nanoseconds();
    for (uint64_t i = 0; i < iterations; ++i) {
        ready = 0;
        ev_timer_set(&timer, 0.0, 0.0);
        ev_timer_start(loop, &timer);
        while (!ready) ev_run(loop, EVRUN_ONCE);
    }
    report("immediate-one-shot-timer", iterations, nanoseconds() - start);
    ev_io_stop(loop, &io);
    ev_loop_destroy(loop);
    close(sockets[0]);
    close(sockets[1]);
    return 0;
}
