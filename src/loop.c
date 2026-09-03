#include "src/internal.h"
#include "src/loop_backend.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct watch_slot {
    int fd;
    unsigned interests;
    maelys_sys_token_t token;
    uint32_t generation;
    int active;
    /* Step serial that last produced an event for this slot, and where that
     * event sits in the caller's array, so a backend reporting directions
     * separately (kqueue) still yields one event per watch and per step. */
    uint64_t seen_step;
    size_t event_index;
} watch_slot_t;

typedef struct timer_slot {
    uint64_t deadline_ms;
    maelys_sys_token_t token;
    uint32_t generation;
    int active;
} timer_slot_t;

typedef struct timer_node {
    uint64_t deadline_ms;
    uint64_t timer_id;
} timer_node_t;

struct maelys_sys_loop {
    pthread_t owner;
    const maelys_sys_loop_backend_ops_t *ops;
    void *backend;
    maelys_sys_wakeup_t *wakeup;
    atomic_int stopped;
    uint64_t step_serial;

    watch_slot_t *watches;
    size_t watch_capacity;
    timer_slot_t *timers;
    size_t timer_capacity;
    timer_node_t *timer_heap;
    size_t timer_heap_count;
    size_t timer_heap_capacity;
    maelys_sys_backend_event_t *raw_events;
    size_t raw_capacity;
};

static int owner_thread(const maelys_sys_loop_t *loop) {
    return loop && pthread_equal(loop->owner, pthread_self());
}

static uint64_t make_id(size_t index, uint32_t generation) {
    return ((uint64_t)generation << 32u) | (uint64_t)(index + 1u);
}

static int split_id(
    uint64_t id, size_t capacity, size_t *out_index, uint32_t *out_generation) {
    uint32_t low = (uint32_t)id;
    uint32_t generation = (uint32_t)(id >> 32u);
    if (!low || !generation || (size_t)low > capacity) return 0;
    *out_index = (size_t)low - 1u;
    *out_generation = generation;
    return 1;
}

static uint32_t next_generation(uint32_t generation) {
    ++generation;
    return generation ? generation : 1u;
}

static const maelys_sys_loop_backend_ops_t *select_backend(
    maelys_sys_loop_backend_t backend) {
    if (backend == MAELYS_SYS_LOOP_POLL) return &maelys_sys_poll_backend_ops;
#ifdef __linux__
    if (backend == MAELYS_SYS_LOOP_AUTO || backend == MAELYS_SYS_LOOP_EPOLL) {
        return &maelys_sys_epoll_backend_ops;
    }
#endif
#ifdef __APPLE__
    if (backend == MAELYS_SYS_LOOP_AUTO || backend == MAELYS_SYS_LOOP_KQUEUE) {
        return &maelys_sys_kqueue_backend_ops;
    }
#endif
    return NULL;
}

int maelys_sys_loop_backend_available(maelys_sys_loop_backend_t backend) {
    if (backend == MAELYS_SYS_LOOP_AUTO || backend == MAELYS_SYS_LOOP_POLL) return 1;
#ifdef __linux__
    if (backend == MAELYS_SYS_LOOP_EPOLL) return 1;
#endif
#ifdef __APPLE__
    if (backend == MAELYS_SYS_LOOP_KQUEUE) return 1;
#endif
    return 0;
}

maelys_sys_result_t maelys_sys_loop_create(
    maelys_sys_loop_backend_t backend,
    maelys_sys_loop_t **out_loop) {
    if (!out_loop) return MAELYS_SYS_ERR_ARGUMENT;
    *out_loop = NULL;
    const maelys_sys_loop_backend_ops_t *ops = select_backend(backend);
    if (!ops) return MAELYS_SYS_ERR_UNSUPPORTED;
    maelys_sys_loop_t *loop = calloc(1, sizeof(*loop));
    if (!loop) return MAELYS_SYS_ERR_MEMORY;
    loop->owner = pthread_self();
    loop->ops = ops;
    atomic_init(&loop->stopped, 0);
    maelys_sys_result_t result = ops->create(&loop->backend);
    if (result != MAELYS_SYS_OK) {
        free(loop);
        return result;
    }
    result = maelys_sys_wakeup_create(&loop->wakeup);
    if (result == MAELYS_SYS_OK) {
        result = ops->add(loop->backend, maelys_sys_wakeup_fd(loop->wakeup),
            MAELYS_SYS_INTEREST_READ, 0);
    }
    if (result != MAELYS_SYS_OK) {
        maelys_sys_wakeup_destroy(loop->wakeup);
        ops->destroy(loop->backend);
        free(loop);
        return result;
    }
    *out_loop = loop;
    return MAELYS_SYS_OK;
}

const char *maelys_sys_loop_backend_name(const maelys_sys_loop_t *loop) {
    return loop && loop->ops ? loop->ops->name : NULL;
}

static maelys_sys_result_t grow_watches(maelys_sys_loop_t *loop) {
    size_t old_capacity = loop->watch_capacity;
    size_t capacity = old_capacity ? old_capacity * 2u : 16u;
    if (capacity < old_capacity || capacity > SIZE_MAX / sizeof(*loop->watches)) {
        return MAELYS_SYS_ERR_CAPACITY;
    }
    watch_slot_t *watches = realloc(loop->watches, capacity * sizeof(*watches));
    if (!watches) return MAELYS_SYS_ERR_MEMORY;
    memset(watches + old_capacity, 0,
        (capacity - old_capacity) * sizeof(*watches));
    for (size_t i = old_capacity; i < capacity; ++i) watches[i].generation = 1u;
    loop->watches = watches;
    loop->watch_capacity = capacity;
    return MAELYS_SYS_OK;
}

static int interests_valid(unsigned interests) {
    unsigned known = MAELYS_SYS_INTEREST_READ | MAELYS_SYS_INTEREST_WRITE;
    return interests && (interests & ~known) == 0;
}

maelys_sys_result_t maelys_sys_loop_watch_fd(
    maelys_sys_loop_t *loop,
    int fd,
    unsigned interests,
    maelys_sys_token_t token,
    maelys_sys_watch_t *out_watch) {
    if (!loop || fd < 0 || !interests_valid(interests) || !out_watch) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (!owner_thread(loop)) return MAELYS_SYS_ERR_STATE;
    for (size_t i = 0; i < loop->watch_capacity; ++i) {
        if (loop->watches[i].active && loop->watches[i].fd == fd) {
            return MAELYS_SYS_ERR_STATE;
        }
    }
    size_t index = loop->watch_capacity;
    for (size_t i = 0; i < loop->watch_capacity; ++i) {
        if (!loop->watches[i].active) { index = i; break; }
    }
    if (index == loop->watch_capacity) {
        maelys_sys_result_t result = grow_watches(loop);
        if (result != MAELYS_SYS_OK) return result;
    }
    watch_slot_t *slot = &loop->watches[index];
    uint64_t id = make_id(index, slot->generation);
    maelys_sys_result_t result =
        loop->ops->add(loop->backend, fd, interests, id);
    if (result != MAELYS_SYS_OK) return result;
    slot->fd = fd;
    slot->interests = interests;
    slot->token = token;
    slot->active = 1;
    *out_watch = id;
    return MAELYS_SYS_OK;
}

static watch_slot_t *find_watch(maelys_sys_loop_t *loop, uint64_t id) {
    size_t index = 0;
    uint32_t generation = 0;
    if (!split_id(id, loop->watch_capacity, &index, &generation)) return NULL;
    watch_slot_t *slot = &loop->watches[index];
    return slot->active && slot->generation == generation ? slot : NULL;
}

maelys_sys_result_t maelys_sys_loop_modify(
    maelys_sys_loop_t *loop,
    maelys_sys_watch_t watch,
    unsigned interests) {
    if (!loop || !interests_valid(interests)) return MAELYS_SYS_ERR_ARGUMENT;
    if (!owner_thread(loop)) return MAELYS_SYS_ERR_STATE;
    watch_slot_t *slot = find_watch(loop, watch);
    if (!slot) return MAELYS_SYS_ERR_NOT_FOUND;
    if (slot->interests == interests) return MAELYS_SYS_OK;
    maelys_sys_result_t result = loop->ops->modify(loop->backend, slot->fd,
        slot->interests, interests, watch);
    if (result == MAELYS_SYS_OK) slot->interests = interests;
    return result;
}

maelys_sys_result_t maelys_sys_loop_unwatch(
    maelys_sys_loop_t *loop,
    maelys_sys_watch_t watch) {
    if (!loop) return MAELYS_SYS_ERR_ARGUMENT;
    if (!owner_thread(loop)) return MAELYS_SYS_ERR_STATE;
    watch_slot_t *slot = find_watch(loop, watch);
    if (!slot) return MAELYS_SYS_ERR_NOT_FOUND;
    maelys_sys_result_t result = loop->ops->remove(
        loop->backend, slot->fd, slot->interests, watch);
    if (result != MAELYS_SYS_OK) return result;
    slot->active = 0;
    slot->fd = -1;
    slot->interests = 0;
    slot->token = 0;
    slot->seen_step = 0;
    slot->event_index = 0;
    slot->generation = next_generation(slot->generation);
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t grow_timers(maelys_sys_loop_t *loop) {
    size_t old_capacity = loop->timer_capacity;
    size_t capacity = old_capacity ? old_capacity * 2u : 16u;
    if (capacity < old_capacity || capacity > SIZE_MAX / sizeof(*loop->timers)) {
        return MAELYS_SYS_ERR_CAPACITY;
    }
    timer_slot_t *timers = realloc(loop->timers, capacity * sizeof(*timers));
    if (!timers) return MAELYS_SYS_ERR_MEMORY;
    memset(timers + old_capacity, 0,
        (capacity - old_capacity) * sizeof(*timers));
    for (size_t i = old_capacity; i < capacity; ++i) timers[i].generation = 1u;
    loop->timers = timers;
    loop->timer_capacity = capacity;
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t grow_heap(maelys_sys_loop_t *loop) {
    if (loop->timer_heap_count < loop->timer_heap_capacity) return MAELYS_SYS_OK;
    size_t capacity = loop->timer_heap_capacity ? loop->timer_heap_capacity * 2u : 16u;
    if (capacity < loop->timer_heap_capacity ||
        capacity > SIZE_MAX / sizeof(*loop->timer_heap)) {
        return MAELYS_SYS_ERR_CAPACITY;
    }
    timer_node_t *heap = realloc(loop->timer_heap, capacity * sizeof(*heap));
    if (!heap) return MAELYS_SYS_ERR_MEMORY;
    loop->timer_heap = heap;
    loop->timer_heap_capacity = capacity;
    return MAELYS_SYS_OK;
}

static void heap_push(maelys_sys_loop_t *loop, timer_node_t node) {
    size_t index = loop->timer_heap_count++;
    while (index > 0) {
        size_t parent = (index - 1u) / 2u;
        if (loop->timer_heap[parent].deadline_ms <= node.deadline_ms) break;
        loop->timer_heap[index] = loop->timer_heap[parent];
        index = parent;
    }
    loop->timer_heap[index] = node;
}

static void heap_pop(maelys_sys_loop_t *loop) {
    if (!loop->timer_heap_count) return;
    timer_node_t last = loop->timer_heap[--loop->timer_heap_count];
    if (!loop->timer_heap_count) return;
    size_t index = 0;
    while (index * 2u + 1u < loop->timer_heap_count) {
        size_t child = index * 2u + 1u;
        if (child + 1u < loop->timer_heap_count &&
            loop->timer_heap[child + 1u].deadline_ms <
                loop->timer_heap[child].deadline_ms) {
            ++child;
        }
        if (loop->timer_heap[child].deadline_ms >= last.deadline_ms) break;
        loop->timer_heap[index] = loop->timer_heap[child];
        index = child;
    }
    loop->timer_heap[index] = last;
}

static timer_slot_t *find_timer(maelys_sys_loop_t *loop, uint64_t id) {
    size_t index = 0;
    uint32_t generation = 0;
    if (!split_id(id, loop->timer_capacity, &index, &generation)) return NULL;
    timer_slot_t *slot = &loop->timers[index];
    return slot->active && slot->generation == generation ? slot : NULL;
}

static void prune_heap(maelys_sys_loop_t *loop) {
    while (loop->timer_heap_count) {
        timer_node_t *node = &loop->timer_heap[0];
        timer_slot_t *slot = find_timer(loop, node->timer_id);
        if (slot && slot->deadline_ms == node->deadline_ms) break;
        heap_pop(loop);
    }
}

maelys_sys_result_t maelys_sys_loop_timer_add(
    maelys_sys_loop_t *loop,
    uint64_t deadline_ms,
    maelys_sys_token_t token,
    maelys_sys_timer_t *out_timer) {
    if (!loop || !out_timer || deadline_ms == MAELYS_SYS_DEADLINE_INFINITE) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (!owner_thread(loop)) return MAELYS_SYS_ERR_STATE;
    size_t index = loop->timer_capacity;
    for (size_t i = 0; i < loop->timer_capacity; ++i) {
        if (!loop->timers[i].active) { index = i; break; }
    }
    if (index == loop->timer_capacity) {
        maelys_sys_result_t result = grow_timers(loop);
        if (result != MAELYS_SYS_OK) return result;
    }
    maelys_sys_result_t result = grow_heap(loop);
    if (result != MAELYS_SYS_OK) return result;
    timer_slot_t *slot = &loop->timers[index];
    uint64_t id = make_id(index, slot->generation);
    slot->deadline_ms = deadline_ms;
    slot->token = token;
    slot->active = 1;
    heap_push(loop, (timer_node_t){.deadline_ms = deadline_ms, .timer_id = id});
    *out_timer = id;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_loop_timer_cancel(
    maelys_sys_loop_t *loop,
    maelys_sys_timer_t timer) {
    if (!loop) return MAELYS_SYS_ERR_ARGUMENT;
    if (!owner_thread(loop)) return MAELYS_SYS_ERR_STATE;
    timer_slot_t *slot = find_timer(loop, timer);
    if (!slot) return MAELYS_SYS_ERR_NOT_FOUND;
    slot->active = 0;
    slot->token = 0;
    slot->generation = next_generation(slot->generation);
    return MAELYS_SYS_OK;
}

static size_t collect_due_timers(
    maelys_sys_loop_t *loop,
    uint64_t now,
    maelys_sys_event_t *events,
    size_t capacity) {
    size_t count = 0;
    prune_heap(loop);
    while (count < capacity && loop->timer_heap_count &&
        loop->timer_heap[0].deadline_ms <= now) {
        timer_node_t node = loop->timer_heap[0];
        timer_slot_t *slot = find_timer(loop, node.timer_id);
        heap_pop(loop);
        if (!slot) { prune_heap(loop); continue; }
        events[count++] = (maelys_sys_event_t){
            .token = slot->token,
            .flags = MAELYS_SYS_EVENT_TIMER
        };
        slot->active = 0;
        slot->token = 0;
        slot->generation = next_generation(slot->generation);
        prune_heap(loop);
    }
    return count;
}

static maelys_sys_result_t reserve_raw(maelys_sys_loop_t *loop, size_t capacity) {
    if (capacity <= loop->raw_capacity) return MAELYS_SYS_OK;
    if (capacity > SIZE_MAX / sizeof(*loop->raw_events)) {
        return MAELYS_SYS_ERR_CAPACITY;
    }
    maelys_sys_backend_event_t *events =
        realloc(loop->raw_events, capacity * sizeof(*events));
    if (!events) return MAELYS_SYS_ERR_MEMORY;
    loop->raw_events = events;
    loop->raw_capacity = capacity;
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t wait_timeout(
    maelys_sys_loop_t *loop,
    uint64_t caller_deadline,
    int *out_timeout) {
    uint64_t now = 0;
    maelys_sys_result_t result = maelys_sys_monotonic_ms(&now);
    if (result != MAELYS_SYS_OK) return result;
    uint64_t effective = caller_deadline;
    prune_heap(loop);
    if (loop->timer_heap_count &&
        (effective == MAELYS_SYS_DEADLINE_INFINITE ||
         loop->timer_heap[0].deadline_ms < effective)) {
        effective = loop->timer_heap[0].deadline_ms;
    }
    if (effective == MAELYS_SYS_DEADLINE_INFINITE) {
        *out_timeout = -1;
    } else if (effective <= now) {
        *out_timeout = 0;
    } else {
        uint64_t remaining = effective - now;
        *out_timeout = remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
    }
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_loop_step(
    maelys_sys_loop_t *loop,
    uint64_t deadline_ms,
    maelys_sys_event_t *events,
    size_t event_capacity,
    size_t *out_event_count,
    maelys_sys_step_result_t *out_step_result) {
    if (!loop || !events || !event_capacity || !out_event_count || !out_step_result) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (!owner_thread(loop)) return MAELYS_SYS_ERR_STATE;
    *out_event_count = 0;
    if (atomic_load_explicit(&loop->stopped, memory_order_acquire)) {
        *out_step_result = MAELYS_SYS_STEP_STOPPED;
        return MAELYS_SYS_OK;
    }
    if (event_capacity == SIZE_MAX) return MAELYS_SYS_ERR_CAPACITY;
    maelys_sys_result_t result = reserve_raw(loop, event_capacity + 1u);
    if (result != MAELYS_SYS_OK) return result;
    for (;;) {
        uint64_t now = 0;
        result = maelys_sys_monotonic_ms(&now);
        if (result != MAELYS_SYS_OK) return result;
        size_t due = collect_due_timers(loop, now, events, event_capacity);
        if (due) {
            *out_event_count = due;
            *out_step_result = MAELYS_SYS_STEP_PROGRESS;
            return MAELYS_SYS_OK;
        }
        int timeout = 0;
        result = wait_timeout(loop, deadline_ms, &timeout);
        if (result != MAELYS_SYS_OK) return result;
        size_t raw_count = 0;
        result = loop->ops->wait(loop->backend, timeout,
            loop->raw_events, event_capacity + 1u, &raw_count);
        if (result == MAELYS_SYS_ERR_OS && errno == EINTR) continue;
        if (result != MAELYS_SYS_OK) return result;
        size_t produced = 0;
        uint64_t serial = ++loop->step_serial;
        for (size_t i = 0; i < raw_count; ++i) {
            maelys_sys_backend_event_t *raw = &loop->raw_events[i];
            if (raw->watch_id == 0) {
                if (produced == event_capacity) {
                    /* No room: leave the pipe readable so the next step
                     * reports the wake instead of consuming and losing it. */
                    continue;
                }
                result = maelys_sys_wakeup_consume(loop->wakeup);
                if (result != MAELYS_SYS_OK) return result;
                if (atomic_load_explicit(&loop->stopped, memory_order_acquire)) {
                    *out_step_result = MAELYS_SYS_STEP_STOPPED;
                    return MAELYS_SYS_OK;
                }
                events[produced++] = (maelys_sys_event_t){
                    .token = 0, .flags = MAELYS_SYS_EVENT_WAKE
                };
                continue;
            }
            watch_slot_t *slot = find_watch(loop, raw->watch_id);
            if (!slot || !raw->flags) continue;
            if (slot->seen_step == serial) {
                events[slot->event_index].flags |= raw->flags;
                continue;
            }
            if (produced == event_capacity) continue;
            slot->seen_step = serial;
            slot->event_index = produced;
            events[produced++] = (maelys_sys_event_t){
                .token = slot->token,
                .flags = raw->flags
            };
        }
        result = maelys_sys_monotonic_ms(&now);
        if (result != MAELYS_SYS_OK) return result;
        if (produced < event_capacity) {
            produced += collect_due_timers(
                loop, now, events + produced, event_capacity - produced);
        }
        if (produced) {
            *out_event_count = produced;
            *out_step_result = MAELYS_SYS_STEP_PROGRESS;
            return MAELYS_SYS_OK;
        }
        if (deadline_ms != MAELYS_SYS_DEADLINE_INFINITE && now >= deadline_ms) {
            *out_step_result = MAELYS_SYS_STEP_TIMEOUT;
            return MAELYS_SYS_OK;
        }
        prune_heap(loop);
        if (loop->timer_heap_count && loop->timer_heap[0].deadline_ms <= now) continue;
        if (timeout == 0) {
            *out_step_result = MAELYS_SYS_STEP_TIMEOUT;
            return MAELYS_SYS_OK;
        }
    }
}

maelys_sys_result_t maelys_sys_loop_wake(maelys_sys_loop_t *loop) {
    return loop ? maelys_sys_wakeup_signal(loop->wakeup) : MAELYS_SYS_ERR_ARGUMENT;
}

maelys_sys_result_t maelys_sys_loop_stop(maelys_sys_loop_t *loop) {
    if (!loop) return MAELYS_SYS_ERR_ARGUMENT;
    atomic_store_explicit(&loop->stopped, 1, memory_order_release);
    return maelys_sys_wakeup_signal(loop->wakeup);
}

maelys_sys_result_t maelys_sys_loop_destroy(maelys_sys_loop_t **loop_pointer) {
    if (!loop_pointer || !*loop_pointer) return MAELYS_SYS_OK;
    maelys_sys_loop_t *loop = *loop_pointer;
    if (!owner_thread(loop)) return MAELYS_SYS_ERR_STATE;
    *loop_pointer = NULL;
    loop->ops->destroy(loop->backend);
    maelys_sys_wakeup_destroy(loop->wakeup);
    free(loop->watches);
    free(loop->timers);
    free(loop->timer_heap);
    free(loop->raw_events);
    free(loop);
    return MAELYS_SYS_OK;
}
