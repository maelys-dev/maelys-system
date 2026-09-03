#define _POSIX_C_SOURCE 200809L

#include "maelys/sys.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_positive(const char *text, uint64_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !end || *end || value == 0u) return 0;
    *out = (uint64_t)value;
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("usage: timer-server TICKS INTERVAL_MS");
        return 0;
    }
    uint64_t ticks = 0;
    uint64_t interval = 0;
    if (argc != 3 || !parse_positive(argv[1], &ticks) ||
        !parse_positive(argv[2], &interval)) {
        fprintf(stderr, "usage: timer-server TICKS INTERVAL_MS\n");
        return 2;
    }

    maelys_sys_loop_t *loop = NULL;
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) != MAELYS_SYS_OK) {
        perror("loop_create");
        return 1;
    }
    uint64_t deadline = 0;
    if (maelys_sys_deadline_after(interval, &deadline) != MAELYS_SYS_OK) return 1;
    maelys_sys_timer_t timer = 0;
    if (maelys_sys_loop_timer_add(loop, deadline, 1, &timer) != MAELYS_SYS_OK) return 1;

    for (uint64_t tick = 1; tick <= ticks;) {
        maelys_sys_event_t events[4];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        if (maelys_sys_loop_step(loop, MAELYS_SYS_DEADLINE_INFINITE,
                events, 4, &count, &step) != MAELYS_SYS_OK) {
            perror("loop_step");
            return 1;
        }
        for (size_t i = 0; i < count; ++i) {
            if (events[i].token != 1 || !(events[i].flags & MAELYS_SYS_EVENT_TIMER)) {
                continue;
            }
            printf("tick %" PRIu64 "\n", tick++);
            fflush(stdout);
            if (tick <= ticks) {
                if (maelys_sys_deadline_after(interval, &deadline) != MAELYS_SYS_OK ||
                    maelys_sys_loop_timer_add(loop, deadline, 1, &timer) !=
                        MAELYS_SYS_OK) return 1;
            }
        }
    }
    return maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK ? 0 : 1;
}
