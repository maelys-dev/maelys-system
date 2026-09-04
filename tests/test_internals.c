/*
 * White-box reactor test. It compiles src/loop.c into this unit to reach the
 * timer heap and its bookkeeping, which no public call exposes: the heap
 * invariant and the count of cancelled nodes are checked after every single
 * operation, against a reference model for the random workload.
 */
#include "src/loop.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint32_t lcg_state = 20260904u;

static uint32_t lcg(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return lcg_state >> 8;
}

static int heap_valid(const maelys_sys_loop_t *loop) {
    for (size_t i = 1; i < loop->timer_heap_count; ++i) {
        if (loop->timer_heap[(i - 1u) / 2u].deadline_ms >
            loop->timer_heap[i].deadline_ms) {
            return 0;
        }
    }
    return 1;
}

static size_t dead_nodes(maelys_sys_loop_t *loop) {
    size_t dead = 0;
    for (size_t i = 0; i < loop->timer_heap_count; ++i) {
        if (!timer_node_live(loop, &loop->timer_heap[i])) ++dead;
    }
    return dead;
}

static int bookkeeping_exact(maelys_sys_loop_t *loop) {
    CHECK(heap_valid(loop));
    CHECK(loop->timer_heap_dead == dead_nodes(loop));
    /* Compaction keeps dead nodes below the live ones once past 64. */
    CHECK(loop->timer_heap_dead < 64u ||
        loop->timer_heap_dead * 2u <= loop->timer_heap_count);
    return 0;
}

/* One timer re-armed 100 000 times: the heap stays bounded by the threshold. */
static int rearm_bounded(void) {
    maelys_sys_loop_t *loop = NULL;
    CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
    uint64_t now = 0;
    CHECK(maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK);
    maelys_sys_timer_t previous = 0;
    size_t max_count = 0;
    for (size_t i = 0; i < 100000u; ++i) {
        maelys_sys_timer_t timer = 0;
        CHECK(maelys_sys_loop_timer_add(loop, now + 60000u + (uint64_t)i,
            (maelys_sys_token_t)(i + 1u), &timer) == MAELYS_SYS_OK);
        if (previous) {
            CHECK(maelys_sys_loop_timer_cancel(loop, previous) == MAELYS_SYS_OK);
        }
        previous = timer;
        if (loop->timer_heap_count > max_count) max_count = loop->timer_heap_count;
        CHECK(bookkeeping_exact(loop) == 0);
    }
    /* One live timer plus at most 64 dead ones before a rebuild. */
    CHECK(max_count <= 66u);
    CHECK(loop->timer_capacity <= 16u);
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

enum {
    MODEL_ITERATIONS = 100000,
    MODEL_MAX_LIVE = 1024,
    MODEL_STEP_CAPACITY = 64,
    MODEL_PAST_SPAN = 1u << 14,
    MODEL_FUTURE_SPAN = 1u << 20
};

typedef struct model_timer {
    maelys_sys_timer_t id;
    uint64_t deadline_ms;
    maelys_sys_token_t token;
} model_timer_t;

static int compare_deadline(const void *left, const void *right) {
    const model_timer_t *a = left;
    const model_timer_t *b = right;
    return a->deadline_ms < b->deadline_ms ? -1 : a->deadline_ms > b->deadline_ms;
}

/*
 * Random add, cancel and step against a sorted reference: every step must
 * fire exactly the due timers with the smallest deadlines, in order, and
 * the heap bookkeeping must be exact after each operation. Past deadlines
 * are drawn below the current clock, distinct, within what the machine's
 * uptime allows.
 */
static int random_model(void) {
    static model_timer_t live[MODEL_MAX_LIVE];
    static unsigned char used_past[MODEL_PAST_SPAN];
    static unsigned char used_future[MODEL_FUTURE_SPAN];
    size_t live_count = 0;
    size_t past_created = 0;
    maelys_sys_token_t token = 0;
    maelys_sys_loop_t *loop = NULL;
    CHECK(maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) == MAELYS_SYS_OK);
    uint64_t base = 0;
    CHECK(maelys_sys_monotonic_ms(&base) == MAELYS_SYS_OK);
    /* A monotonic clock this young cannot host past deadlines. */
    CHECK(base >= MODEL_PAST_SPAN);
    for (size_t iteration = 0; iteration < MODEL_ITERATIONS; ++iteration) {
        uint32_t roll = lcg() % 100u;
        if (roll < 45u && live_count < MODEL_MAX_LIVE) {
            int past = (lcg() & 1u) != 0 &&
                past_created < (MODEL_PAST_SPAN / 4u) * 3u;
            unsigned char *used = past ? used_past : used_future;
            uint32_t span = past ? MODEL_PAST_SPAN : MODEL_FUTURE_SPAN;
            uint32_t offset;
            do { offset = lcg() % span; } while (used[offset]);
            used[offset] = 1;
            if (past) ++past_created;
            uint64_t deadline = past ? base - 1u - offset :
                base + 5000000u + offset;
            maelys_sys_timer_t id = 0;
            CHECK(maelys_sys_loop_timer_add(loop, deadline, ++token, &id) ==
                MAELYS_SYS_OK);
            live[live_count++] = (model_timer_t){id, deadline, token};
        } else if (roll < 85u && live_count) {
            size_t k = lcg() % (uint32_t)live_count;
            CHECK(maelys_sys_loop_timer_cancel(loop, live[k].id) == MAELYS_SYS_OK);
            CHECK(maelys_sys_loop_timer_cancel(loop, live[k].id) ==
                MAELYS_SYS_ERR_NOT_FOUND);
            live[k] = live[--live_count];
        } else {
            qsort(live, live_count, sizeof(live[0]), compare_deadline);
            size_t due = 0;
            while (due < live_count && live[due].deadline_ms < base) ++due;
            size_t expected = due < MODEL_STEP_CAPACITY ? due : MODEL_STEP_CAPACITY;
            maelys_sys_event_t events[MODEL_STEP_CAPACITY];
            size_t count = 0;
            maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
            CHECK(maelys_sys_loop_step(loop, base - 1u, events,
                MODEL_STEP_CAPACITY, &count, &step) == MAELYS_SYS_OK);
            CHECK(count == expected);
            CHECK(step == (expected ? MAELYS_SYS_STEP_PROGRESS :
                MAELYS_SYS_STEP_TIMEOUT));
            for (size_t i = 0; i < count; ++i) {
                CHECK(events[i].flags == MAELYS_SYS_EVENT_TIMER);
                CHECK(events[i].token == live[i].token);
            }
            memmove(live, live + count, (live_count - count) * sizeof(live[0]));
            live_count -= count;
        }
        CHECK(bookkeeping_exact(loop) == 0);
    }
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

int main(void) {
    if (rearm_bounded() != 0) return 1;
    if (random_model() != 0) return 1;
    puts("ok - timer heap bounded under re-arm");
    puts("ok - timer heap bookkeeping against a reference model");
    return 0;
}
