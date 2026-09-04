#include "maelys/sys.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

enum {
    PAIR_COUNT = 128,
    TIMER_COUNT = 2048,
    GENERATION_COUNT = 10000,
    REARM_COUNT = 100000
};

static int readiness_stress(maelys_sys_loop_backend_t backend) {
    maelys_sys_loop_t *loop = NULL;
    int pairs[PAIR_COUNT][2];
    maelys_sys_watch_t watches[PAIR_COUNT];
    unsigned char seen[PAIR_COUNT] = {0};
    for (size_t i = 0; i < PAIR_COUNT; ++i) {
        pairs[i][0] = -1;
        pairs[i][1] = -1;
    }
    CHECK(maelys_sys_loop_create(backend, &loop) == MAELYS_SYS_OK);
    for (size_t i = 0; i < PAIR_COUNT; ++i) {
        CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, pairs[i]) ==
            MAELYS_SYS_OK);
        CHECK(maelys_sys_loop_watch_fd(loop, pairs[i][0],
            MAELYS_SYS_INTEREST_READ, (maelys_sys_token_t)(i + 1u),
            &watches[i]) == MAELYS_SYS_OK);
        CHECK(write(pairs[i][1], "x", 1) == 1);
    }
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(5000, &deadline) == MAELYS_SYS_OK);
    size_t total = 0;
    while (total < PAIR_COUNT) {
        maelys_sys_event_t events[13];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        CHECK(maelys_sys_loop_step(loop, deadline, events, 13, &count, &step) ==
            MAELYS_SYS_OK);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS);
        for (size_t i = 0; i < count; ++i) {
            if (!(events[i].flags & MAELYS_SYS_EVENT_READ) || events[i].token == 0 ||
                events[i].token > PAIR_COUNT) continue;
            size_t index = (size_t)events[i].token - 1u;
            if (seen[index]) continue;
            char byte = 0;
            CHECK(read(pairs[index][0], &byte, 1) == 1 && byte == 'x');
            seen[index] = 1;
            ++total;
        }
    }
    for (size_t i = 0; i < PAIR_COUNT; ++i) {
        CHECK(maelys_sys_loop_unwatch(loop, watches[i]) == MAELYS_SYS_OK);
        CHECK(maelys_sys_fd_close(&pairs[i][0]) == MAELYS_SYS_OK);
        CHECK(maelys_sys_fd_close(&pairs[i][1]) == MAELYS_SYS_OK);
    }
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

static int timer_and_generation_stress(void) {
    maelys_sys_loop_t *loop = NULL;
    CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
    uint64_t now = 0;
    CHECK(maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK);
    for (size_t i = 0; i < TIMER_COUNT; ++i) {
        maelys_sys_timer_t timer = 0;
        CHECK(maelys_sys_loop_timer_add(
            loop, now, (maelys_sys_token_t)(i + 1u), &timer) == MAELYS_SYS_OK);
        if ((i & 1u) != 0) {
            CHECK(maelys_sys_loop_timer_cancel(loop, timer) == MAELYS_SYS_OK);
        }
    }
    size_t timers_seen = 0;
    while (timers_seen < TIMER_COUNT / 2u) {
        maelys_sys_event_t events[31];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        CHECK(maelys_sys_loop_step(loop, now, events, 31, &count, &step) ==
            MAELYS_SYS_OK);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS);
        for (size_t i = 0; i < count; ++i) {
            CHECK(events[i].flags == MAELYS_SYS_EVENT_TIMER);
            CHECK((events[i].token & 1u) != 0);
        }
        timers_seen += count;
    }

    int pair[2] = {-1, -1};
    CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, pair) == MAELYS_SYS_OK);
    maelys_sys_watch_t previous = 0;
    for (size_t i = 0; i < GENERATION_COUNT; ++i) {
        maelys_sys_watch_t watch = 0;
        CHECK(maelys_sys_loop_watch_fd(loop, pair[0], MAELYS_SYS_INTEREST_READ,
            (maelys_sys_token_t)i, &watch) == MAELYS_SYS_OK);
        CHECK(watch != previous);
        if (previous) {
            /* The freed slot is reused: same index, next generation. */
            CHECK((uint32_t)watch == (uint32_t)previous);
            CHECK(maelys_sys_loop_unwatch(loop, previous) ==
                MAELYS_SYS_ERR_NOT_FOUND);
        }
        CHECK(maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_OK);
        previous = watch;
    }
    CHECK(maelys_sys_fd_close(&pair[0]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&pair[1]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

/*
 * A timeout re-armed on every packet: add a far timer, cancel the previous
 * one, many times. The heap must not keep every cancelled node, and the
 * one live timer must still fire first when it is due.
 */
static int timer_rearm_stress(void) {
    maelys_sys_loop_t *loop = NULL;
    CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
    uint64_t now = 0;
    CHECK(maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK);
    maelys_sys_timer_t previous = 0;
    for (size_t i = 0; i < REARM_COUNT; ++i) {
        maelys_sys_timer_t timer = 0;
        CHECK(maelys_sys_loop_timer_add(loop, now + 60000u + (uint64_t)i,
            (maelys_sys_token_t)(i + 1u), &timer) == MAELYS_SYS_OK);
        if (previous) {
            CHECK(maelys_sys_loop_timer_cancel(loop, previous) == MAELYS_SYS_OK);
        }
        previous = timer;
    }
    maelys_sys_timer_t due = 0;
    CHECK(maelys_sys_loop_timer_add(loop, now, 7, &due) == MAELYS_SYS_OK);
    maelys_sys_event_t event;
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
    CHECK(maelys_sys_loop_step(loop, now, &event, 1, &count, &step) ==
        MAELYS_SYS_OK);
    CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == 1);
    CHECK(event.token == 7 && event.flags == MAELYS_SYS_EVENT_TIMER);
    /* The surviving far timer is still live, every cancelled one is gone. */
    CHECK(maelys_sys_loop_timer_cancel(loop, previous) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_step(loop, now, &event, 1, &count, &step) ==
        MAELYS_SYS_OK);
    CHECK(step == MAELYS_SYS_STEP_TIMEOUT && count == 0);
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

/*
 * Thousands of due timers inserted in shuffled order, most of them
 * cancelled so dead nodes sit at interior heap positions and force a
 * compaction; every survivor must then fire in deadline order. A rebuilt
 * heap that lost its invariant fires out of order.
 */
static int timer_compaction_order(void) {
    enum { ORDER_COUNT = 4000 };
    static maelys_sys_timer_t timers[ORDER_COUNT];
    maelys_sys_loop_t *loop = NULL;
    CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
    uint64_t now = 0;
    CHECK(maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK);
    uint64_t base = now >= ORDER_COUNT ? now - ORDER_COUNT : 0u;
    for (size_t i = 0; i < ORDER_COUNT; ++i) {
        /* An odd multiplier coprime with ORDER_COUNT: a permutation. */
        uint64_t deadline = base + (uint64_t)((i * 2654435761u) % ORDER_COUNT);
        CHECK(maelys_sys_loop_timer_add(
            loop, deadline, deadline, &timers[i]) == MAELYS_SYS_OK);
    }
    uint32_t seed = 12345u;
    size_t expected = ORDER_COUNT;
    for (size_t i = 0; i < ORDER_COUNT; ++i) {
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 5u < 3u) {
            CHECK(maelys_sys_loop_timer_cancel(loop, timers[i]) == MAELYS_SYS_OK);
            --expected;
        }
    }
    uint64_t previous = 0;
    size_t fired = 0;
    while (fired < expected) {
        maelys_sys_event_t events[7];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        CHECK(maelys_sys_loop_step(loop, base + ORDER_COUNT + 1000u,
            events, 7, &count, &step) == MAELYS_SYS_OK);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS);
        for (size_t i = 0; i < count; ++i) {
            CHECK(events[i].flags == MAELYS_SYS_EVENT_TIMER);
            CHECK(events[i].token >= previous);
            previous = events[i].token;
        }
        fired += count;
    }
    CHECK(fired == expected);
    maelys_sys_event_t event;
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_PROGRESS;
    CHECK(maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_step(loop, now, &event, 1, &count, &step) ==
        MAELYS_SYS_OK);
    CHECK(step == MAELYS_SYS_STEP_TIMEOUT && count == 0);
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

int main(void) {
    if (readiness_stress(MAELYS_SYS_LOOP_POLL) != 0) return 1;
    if (readiness_stress(MAELYS_SYS_LOOP_AUTO) != 0) return 1;
    if (timer_and_generation_stress() != 0) return 1;
    if (timer_rearm_stress() != 0) return 1;
    if (timer_compaction_order() != 0) return 1;
    puts("ok - readiness stress");
    puts("ok - timer and generation stress");
    puts("ok - timer re-arm stress");
    puts("ok - timer compaction order");
    return 0;
}
