#define _POSIX_C_SOURCE 200809L

#include <event2/event.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct event_context {
    int ready;
} event_context_t;

static void mark_ready(evutil_socket_t fd, short flags, void *opaque) {
    (void)fd;
    (void)flags;
    ((event_context_t *)opaque)->ready = 1;
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
    printf("libevent,%s,%" PRIu64 ",%" PRIu64 ",%.2f\n",
        workload, count, elapsed, (double)elapsed / (double)count);
}

int main(int argc, char **argv) {
    uint64_t iterations = iterations_from(argc, argv);
    if (!iterations) return 2;
    struct event_base *base = event_base_new();
    int sockets[2];
    if (!base || socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return 1;
    event_context_t context = {0};
    struct event *read_event = event_new(base, sockets[0], EV_READ | EV_PERSIST,
        mark_ready, &context);
    if (!read_event || event_add(read_event, NULL) != 0) return 1;
    uint64_t start = nanoseconds();
    for (uint64_t i = 0; i < iterations; ++i) {
        context.ready = 0;
        if (write(sockets[1], "x", 1) != 1) return 1;
        while (!context.ready && event_base_loop(base, EVLOOP_ONCE) == 0) {}
        char byte;
        if (!context.ready || read(sockets[0], &byte, 1) != 1) return 1;
    }
    report("readiness-roundtrip", iterations, nanoseconds() - start);
    struct timeval immediate = {0, 0};
    struct event *timer = evtimer_new(base, mark_ready, &context);
    if (!timer) return 1;
    start = nanoseconds();
    for (uint64_t i = 0; i < iterations; ++i) {
        context.ready = 0;
        if (evtimer_add(timer, &immediate) != 0) return 1;
        while (!context.ready && event_base_loop(base, EVLOOP_ONCE) == 0) {}
        if (!context.ready) return 1;
    }
    report("immediate-one-shot-timer", iterations, nanoseconds() - start);
    event_free(timer);
    event_free(read_event);
    event_base_free(base);
    close(sockets[0]);
    close(sockets[1]);
    return 0;
}
