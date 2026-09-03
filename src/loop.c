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
    uint32_t next_free; /* free-list link, index + 1, 0 ends the list */
} watch_slot_t;

typedef struct timer_slot {
    uint64_t deadline_ms;
    maelys_sys_token_t token;
    uint32_t generation;
    int active;
    uint32_t next_free;
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
    uint32_t free_watch;
    uint32_t *fd_slots; /* fd -> slot index + 1, 0 when unwatched */
    size_t fd_slot_capacity;
    timer_slot_t *timers;
    size_t timer_capacity;
    uint32_t free_timer;
    timer_node_t *timer_heap;
    size_t timer_heap_count;
    size_t timer_heap_capacity;
    size_t timer_heap_dead; /* nodes whose timer was cancelled */
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

typedef struct backend_entry {
    maelys_sys_loop_backend_t backend;
    const maelys_sys_loop_backend_ops_t *ops;
} backend_entry_t;

/* The native backend comes first so AUTO selects it; poll is the fallback. */
static const backend_entry_t backends[] = {
#ifdef __linux__
    {MAELYS_SYS_LOOP_EPOLL, &maelys_sys_epoll_backend_ops},
#endif
#ifdef __APPLE__
    {MAELYS_SYS_LOOP_KQUEUE, &maelys_sys_kqueue_backend_ops},
#endif
    {MAELYS_SYS_LOOP_POLL, &maelys_sys_poll_backend_ops}
};

static const maelys_sys_loop_backend_ops_t *select_backend(
    maelys_sys_loop_backend_t backend) {
    if (backend == MAELYS_SYS_LOOP_AUTO) return backends[0].ops;
    for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); ++i) {
        if (backends[i].backend == backend) return backends[i].ops;
    }
    return NULL;
}

int maelys_sys_loop_backend_available(maelys_sys_loop_backend_t backend) {
    return select_backend(backend) != NULL;
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

/* Slot indices travel in the low 32 bits of an id: capacity stays below. */
static int slot_capacity_valid(size_t old_capacity, size_t capacity, size_t size) {
    return capacity > old_capacity && capacity < UINT32_MAX &&
        capacity <= SIZE_MAX / size;
}

static maelys_sys_result_t grow_watches(maelys_sys_loop_t *loop) {
    size_t old_capacity = loop->watch_capacity;
    size_t capacity = old_capacity ? old_capacity * 2u : 16u;
    if (!slot_capacity_valid(old_capacity, capacity, sizeof(*loop->watches))) {
        return MAELYS_SYS_ERR_CAPACITY;
    }
    watch_slot_t *watches = realloc(loop->watches, capacity * sizeof(*watches));
    if (!watches) return MAELYS_SYS_ERR_MEMORY;
    memset(watches + old_capacity, 0,
        (capacity - old_capacity) * sizeof(*watches));
    for (size_t i = old_capacity; i < capacity; ++i) {
        watches[i].fd = -1;
        watches[i].generation = 1u;
        watches[i].next_free =
            i + 1u < capacity ? (uint32_t)(i + 2u) : loop->free_watch;
    }
    loop->free_watch = (uint32_t)(old_capacity + 1u);
    loop->watches = watches;
    loop->watch_capacity = capacity;
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t reserve_fd_slot(maelys_sys_loop_t *loop, int fd) {
    size_t needed = (size_t)fd + 1u;
    if (needed <= loop->fd_slot_capacity) return MAELYS_SYS_OK;
    size_t capacity = loop->fd_slot_capacity ? loop->fd_slot_capacity : 64u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) return MAELYS_SYS_ERR_CAPACITY;
        capacity *= 2u;
    }
    if (capacity > SIZE_MAX / sizeof(*loop->fd_slots)) {
        return MAELYS_SYS_ERR_CAPACITY;
    }
    uint32_t *slots = realloc(loop->fd_slots, capacity * sizeof(*slots));
    if (!slots) return MAELYS_SYS_ERR_MEMORY;
    memset(slots + loop->fd_slot_capacity, 0,
        (capacity - loop->fd_slot_capacity) * sizeof(*slots));
    loop->fd_slots = slots;
    loop->fd_slot_capacity = capacity;
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
    if ((size_t)fd < loop->fd_slot_capacity && loop->fd_slots[fd]) {
        return MAELYS_SYS_ERR_STATE;
    }
    maelys_sys_result_t result = reserve_fd_slot(loop, fd);
    if (result != MAELYS_SYS_OK) return result;
    if (!loop->free_watch) {
        result = grow_watches(loop);
        if (result != MAELYS_SYS_OK) return result;
    }
    size_t index = (size_t)loop->free_watch - 1u;
    watch_slot_t *slot = &loop->watches[index];
    uint64_t id = make_id(index, slot->generation);
    result = loop->ops->add(loop->backend, fd, interests, id);
    if (result != MAELYS_SYS_OK) return result;
    loop->free_watch = slot->next_free;
    slot->next_free = 0;
    slot->fd = fd;
    slot->interests = interests;
    slot->token = token;
    slot->active = 1;
    loop->fd_slots[fd] = (uint32_t)(index + 1u);
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
    loop->fd_slots[slot->fd] = 0;
    slot->active = 0;
    slot->fd = -1;
    slot->interests = 0;
    slot->token = 0;
    slot->seen_step = 0;
    slot->event_index = 0;
    slot->generation = next_generation(slot->generation);
    slot->next_free = loop->free_watch;
    loop->free_watch = (uint32_t)((size_t)(slot - loop->watches) + 1u);
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t grow_timers(maelys_sys_loop_t *loop) {
    size_t old_capacity = loop->timer_capacity;
    size_t capacity = old_capacity ? old_capacity * 2u : 16u;
    if (!slot_capacity_valid(old_capacity, capacity, sizeof(*loop->timers))) {
        return MAELYS_SYS_ERR_CAPACITY;
    }
    timer_slot_t *timers = realloc(loop->timers, capacity * sizeof(*timers));
    if (!timers) return MAELYS_SYS_ERR_MEMORY;
    memset(timers + old_capacity, 0,
        (capacity - old_capacity) * sizeof(*timers));
    for (size_t i = old_capacity; i < capacity; ++i) {
        timers[i].generation = 1u;
        timers[i].next_free =
            i + 1u < capacity ? (uint32_t)(i + 2u) : loop->free_timer;
    }
    loop->free_timer = (uint32_t)(old_capacity + 1u);
    loop->timers = timers;
    loop->timer_capacity = capacity;
    return MAELYS_SYS_OK;
}

static void release_timer_slot(maelys_sys_loop_t *loop, timer_slot_t *slot) {
    slot->active = 0;
    slot->token = 0;
    slot->generation = next_generation(slot->generation);
    slot->next_free = loop->free_timer;
    loop->free_timer = (uint32_t)((size_t)(slot - loop->timers) + 1u);
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

static void heap_sift_down(maelys_sys_loop_t *loop, size_t index) {
    timer_node_t node = loop->timer_heap[index];
    while (index * 2u + 1u < loop->timer_heap_count) {
        size_t child = index * 2u + 1u;
        if (child + 1u < loop->timer_heap_count &&
            loop->timer_heap[child + 1u].deadline_ms <
                loop->timer_heap[child].deadline_ms) {
            ++child;
        }
        if (loop->timer_heap[child].deadline_ms >= node.deadline_ms) break;
        loop->timer_heap[index] = loop->timer_heap[child];
        index = child;
    }
    loop->timer_heap[index] = node;
}

static void heap_pop(maelys_sys_loop_t *loop) {
    if (!loop->timer_heap_count) return;
    timer_node_t last = loop->timer_heap[--loop->timer_heap_count];
    if (!loop->timer_heap_count) return;
    loop->timer_heap[0] = last;
    heap_sift_down(loop, 0);
}

static timer_slot_t *find_timer(maelys_sys_loop_t *loop, uint64_t id) {
    size_t index = 0;
    uint32_t generation = 0;
    if (!split_id(id, loop->timer_capacity, &index, &generation)) return NULL;
    timer_slot_t *slot = &loop->timers[index];
    return slot->active && slot->generation == generation ? slot : NULL;
}

static int timer_node_live(maelys_sys_loop_t *loop, const timer_node_t *node) {
    timer_slot_t *slot = find_timer(loop, node->timer_id);
    return slot && slot->deadline_ms == node->deadline_ms;
}

static void prune_heap(maelys_sys_loop_t *loop) {
    while (loop->timer_heap_count) {
        if (timer_node_live(loop, &loop->timer_heap[0])) break;
        heap_pop(loop);
        if (loop->timer_heap_dead) --loop->timer_heap_dead;
    }
}

/*
 * Cancelled timers leave their node in the heap until it reaches the top.
 * A re-armed timeout (cancel then add on every packet) would otherwise
 * grow the heap with the traffic rather than with the live timers, so the
 * heap is rebuilt without dead nodes once they outnumber the live ones.
 */
static void heap_compact(maelys_sys_loop_t *loop) {
    size_t kept = 0;
    for (size_t i = 0; i < loop->timer_heap_count; ++i) {
        if (timer_node_live(loop, &loop->timer_heap[i])) {
            loop->timer_heap[kept++] = loop->timer_heap[i];
        }
    }
    loop->timer_heap_count = kept;
    loop->timer_heap_dead = 0;
    for (size_t i = kept / 2u; i-- > 0;) heap_sift_down(loop, i);
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
    if (!loop->free_timer) {
        maelys_sys_result_t result = grow_timers(loop);
        if (result != MAELYS_SYS_OK) return result;
    }
    maelys_sys_result_t result = grow_heap(loop);
    if (result != MAELYS_SYS_OK) return result;
    size_t index = (size_t)loop->free_timer - 1u;
    timer_slot_t *slot = &loop->timers[index];
    uint64_t id = make_id(index, slot->generation);
    loop->free_timer = slot->next_free;
    slot->next_free = 0;
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
    release_timer_slot(loop, slot);
    ++loop->timer_heap_dead;
    if (loop->timer_heap_dead >= 64u &&
        loop->timer_heap_dead * 2u > loop->timer_heap_count) {
        heap_compact(loop);
    }
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
        if (!slot) {
            if (loop->timer_heap_dead) --loop->timer_heap_dead;
            prune_heap(loop);
            continue;
        }
        events[count++] = (maelys_sys_event_t){
            .token = slot->token,
            .flags = MAELYS_SYS_EVENT_TIMER
        };
        release_timer_slot(loop, slot);
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
    free(loop->fd_slots);
    free(loop->timers);
    free(loop->timer_heap);
    free(loop->raw_events);
    free(loop);
    return MAELYS_SYS_OK;
}
