#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "src/loop_backend.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

typedef struct poll_entry {
    struct pollfd descriptor;
    uint64_t watch_id;
} poll_entry_t;

typedef struct poll_context {
    poll_entry_t *entries;
    struct pollfd *scratch;
    size_t count;
    size_t capacity;
    size_t scratch_capacity;
} poll_context_t;

static short poll_interests(unsigned interests) {
    short events = 0;
#ifdef POLLRDHUP
    /* Peer half-close surfaces as HUP, as epoll (EPOLLRDHUP) and kqueue
     * (EV_EOF) already report it. */
    events = (short)(events | POLLRDHUP);
#endif
    if (interests & MAELYS_SYS_INTEREST_READ) events |= POLLIN;
    if (interests & MAELYS_SYS_INTEREST_WRITE) events |= POLLOUT;
    return events;
}

static maelys_sys_result_t poll_create(void **out_context) {
    if (!out_context) return MAELYS_SYS_ERR_ARGUMENT;
    *out_context = calloc(1, sizeof(poll_context_t));
    return *out_context ? MAELYS_SYS_OK : MAELYS_SYS_ERR_MEMORY;
}

static void poll_destroy(void *opaque) {
    poll_context_t *context = opaque;
    if (!context) return;
    free(context->entries);
    free(context->scratch);
    free(context);
}

static maelys_sys_result_t poll_reserve(poll_context_t *context) {
    if (context->count < context->capacity) return MAELYS_SYS_OK;
    size_t capacity = context->capacity ? context->capacity * 2u : 16u;
    if (capacity < context->capacity ||
        capacity > SIZE_MAX / sizeof(*context->entries)) {
        return MAELYS_SYS_ERR_CAPACITY;
    }
    poll_entry_t *entries = realloc(context->entries, capacity * sizeof(*entries));
    if (!entries) return MAELYS_SYS_ERR_MEMORY;
    context->entries = entries;
    context->capacity = capacity;
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t poll_add(
    void *opaque, int fd, unsigned interests, uint64_t watch_id) {
    poll_context_t *context = opaque;
    if (!context || fd < 0 || !interests) return MAELYS_SYS_ERR_ARGUMENT;
    maelys_sys_result_t result = poll_reserve(context);
    if (result != MAELYS_SYS_OK) return result;
    context->entries[context->count++] = (poll_entry_t){
        .descriptor = {.fd = fd, .events = poll_interests(interests), .revents = 0},
        .watch_id = watch_id
    };
    return MAELYS_SYS_OK;
}

static poll_entry_t *poll_find(poll_context_t *context, uint64_t watch_id) {
    for (size_t i = 0; i < context->count; ++i) {
        if (context->entries[i].watch_id == watch_id) return &context->entries[i];
    }
    return NULL;
}

static maelys_sys_result_t poll_modify(
    void *opaque, int fd, unsigned old_interests,
    unsigned new_interests, uint64_t watch_id) {
    (void)old_interests;
    poll_context_t *context = opaque;
    poll_entry_t *entry = context ? poll_find(context, watch_id) : NULL;
    if (!entry || entry->descriptor.fd != fd) return MAELYS_SYS_ERR_NOT_FOUND;
    entry->descriptor.events = poll_interests(new_interests);
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t poll_remove(
    void *opaque, int fd, unsigned interests, uint64_t watch_id) {
    (void)interests;
    poll_context_t *context = opaque;
    if (!context) return MAELYS_SYS_ERR_ARGUMENT;
    for (size_t i = 0; i < context->count; ++i) {
        if (context->entries[i].watch_id == watch_id &&
            context->entries[i].descriptor.fd == fd) {
            if (i + 1u < context->count) {
                memmove(&context->entries[i], &context->entries[i + 1u],
                    (context->count - i - 1u) * sizeof(*context->entries));
            }
            --context->count;
            return MAELYS_SYS_OK;
        }
    }
    return MAELYS_SYS_ERR_NOT_FOUND;
}

static unsigned poll_flags(short revents) {
    unsigned flags = 0;
    if (revents & (POLLIN | POLLPRI)) flags |= MAELYS_SYS_EVENT_READ;
    if (revents & POLLOUT) flags |= MAELYS_SYS_EVENT_WRITE;
    if (revents & POLLHUP) flags |= MAELYS_SYS_EVENT_HUP;
#ifdef POLLRDHUP
    if (revents & POLLRDHUP) flags |= MAELYS_SYS_EVENT_HUP;
#endif
    if (revents & (POLLERR | POLLNVAL)) flags |= MAELYS_SYS_EVENT_ERROR;
    return flags;
}

static maelys_sys_result_t poll_wait(
    void *opaque, int timeout_ms,
    maelys_sys_backend_event_t *events, size_t capacity,
    size_t *out_count) {
    poll_context_t *context = opaque;
    if (!context || !events || !capacity || !out_count) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    *out_count = 0;
    if (context->count > (size_t)((nfds_t)-1)) return MAELYS_SYS_ERR_CAPACITY;
    if (context->count > context->scratch_capacity) {
        if (context->count > SIZE_MAX / sizeof(*context->scratch)) {
            return MAELYS_SYS_ERR_CAPACITY;
        }
        struct pollfd *scratch = realloc(
            context->scratch, context->count * sizeof(*context->scratch));
        if (!scratch) return MAELYS_SYS_ERR_MEMORY;
        context->scratch = scratch;
        context->scratch_capacity = context->count;
    }
    for (size_t i = 0; i < context->count; ++i) {
        context->scratch[i] = context->entries[i].descriptor;
        context->scratch[i].revents = 0;
    }
    int ready = poll(context->scratch, (nfds_t)context->count, timeout_ms);
    if (ready < 0) {
        return MAELYS_SYS_ERR_OS;
    }
    if (ready > 0) {
        for (size_t i = 0; i < context->count && *out_count < capacity; ++i) {
            unsigned flags = poll_flags(context->scratch[i].revents);
            if (!flags) continue;
            events[*out_count] = (maelys_sys_backend_event_t){
                .watch_id = context->entries[i].watch_id,
                .flags = flags
            };
            ++*out_count;
        }
    }
    return MAELYS_SYS_OK;
}

const maelys_sys_loop_backend_ops_t maelys_sys_poll_backend_ops = {
    .name = "poll",
    .create = poll_create,
    .destroy = poll_destroy,
    .add = poll_add,
    .modify = poll_modify,
    .remove = poll_remove,
    .wait = poll_wait
};
