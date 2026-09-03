#ifdef __APPLE__

#include "src/loop_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct kqueue_context {
    int fd;
    struct kevent *events;
    size_t event_capacity;
} kqueue_context_t;

static maelys_sys_result_t kqueue_create_backend(void **out_context) {
    if (!out_context) return MAELYS_SYS_ERR_ARGUMENT;
    *out_context = NULL;
    kqueue_context_t *context = calloc(1, sizeof(*context));
    if (!context) return MAELYS_SYS_ERR_MEMORY;
    context->fd = kqueue();
    if (context->fd < 0) {
        free(context);
        return MAELYS_SYS_ERR_OS;
    }
    int flags = fcntl(context->fd, F_GETFD);
    if (flags < 0 || fcntl(context->fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        int saved = errno;
        close(context->fd);
        free(context);
        errno = saved;
        return MAELYS_SYS_ERR_OS;
    }
    *out_context = context;
    return MAELYS_SYS_OK;
}

static void kqueue_destroy_backend(void *opaque) {
    kqueue_context_t *context = opaque;
    if (!context) return;
    if (context->fd >= 0) close(context->fd);
    free(context->events);
    free(context);
}

static maelys_sys_result_t kqueue_change(
    kqueue_context_t *context, int fd, int16_t filter,
    uint16_t flags, uint64_t watch_id) {
    struct kevent change;
    EV_SET(&change, (uintptr_t)fd, filter, flags, 0, 0,
        (void *)(uintptr_t)watch_id);
    if (kevent(context->fd, &change, 1, NULL, 0, NULL) != 0) {
        if ((flags & EV_DELETE) && errno == ENOENT) {
            return MAELYS_SYS_ERR_NOT_FOUND;
        }
        return MAELYS_SYS_ERR_OS;
    }
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t kqueue_add(
    void *opaque, int fd, unsigned interests, uint64_t watch_id) {
    kqueue_context_t *context = opaque;
    if (interests & MAELYS_SYS_INTEREST_READ) {
        maelys_sys_result_t result = kqueue_change(
            context, fd, EVFILT_READ, EV_ADD | EV_ENABLE, watch_id);
        if (result != MAELYS_SYS_OK) return result;
    }
    if (interests & MAELYS_SYS_INTEREST_WRITE) {
        maelys_sys_result_t result = kqueue_change(
            context, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, watch_id);
        if (result != MAELYS_SYS_OK) {
            if (interests & MAELYS_SYS_INTEREST_READ) {
                (void)kqueue_change(context, fd, EVFILT_READ, EV_DELETE, watch_id);
            }
            return result;
        }
    }
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t kqueue_modify(
    void *opaque, int fd, unsigned old_interests,
    unsigned new_interests, uint64_t watch_id) {
    kqueue_context_t *context = opaque;
    unsigned added = new_interests & ~old_interests;
    unsigned removed = old_interests & ~new_interests;
    int16_t added_filter = 0;
    if (added & MAELYS_SYS_INTEREST_READ) added_filter = EVFILT_READ;
    if (added & MAELYS_SYS_INTEREST_WRITE) added_filter = EVFILT_WRITE;
    if (added_filter) {
        maelys_sys_result_t result = kqueue_change(
            context, fd, added_filter, EV_ADD | EV_ENABLE, watch_id);
        if (result != MAELYS_SYS_OK) return result;
    }
    int16_t removed_filter = 0;
    if (removed & MAELYS_SYS_INTEREST_READ) removed_filter = EVFILT_READ;
    if (removed & MAELYS_SYS_INTEREST_WRITE) removed_filter = EVFILT_WRITE;
    if (removed_filter) {
        maelys_sys_result_t result = kqueue_change(
            context, fd, removed_filter, EV_DELETE, watch_id);
        if (result != MAELYS_SYS_OK) {
            if (added_filter) {
                (void)kqueue_change(
                    context, fd, added_filter, EV_DELETE, watch_id);
            }
            return result;
        }
    }
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t kqueue_remove(
    void *opaque, int fd, unsigned interests, uint64_t watch_id) {
    kqueue_context_t *context = opaque;
    maelys_sys_result_t result = MAELYS_SYS_OK;
    if (interests & MAELYS_SYS_INTEREST_READ) {
        result = kqueue_change(context, fd, EVFILT_READ, EV_DELETE, watch_id);
        if (result == MAELYS_SYS_ERR_NOT_FOUND) result = MAELYS_SYS_OK;
    }
    if (result == MAELYS_SYS_OK && interests & MAELYS_SYS_INTEREST_WRITE) {
        result = kqueue_change(context, fd, EVFILT_WRITE, EV_DELETE, watch_id);
        if (result == MAELYS_SYS_ERR_NOT_FOUND) result = MAELYS_SYS_OK;
        if (result != MAELYS_SYS_OK && interests & MAELYS_SYS_INTEREST_READ) {
            (void)kqueue_change(
                context, fd, EVFILT_READ, EV_ADD | EV_ENABLE, watch_id);
        }
    }
    return result;
}

static maelys_sys_result_t kqueue_wait_backend(
    void *opaque, int timeout_ms,
    maelys_sys_backend_event_t *events, size_t capacity,
    size_t *out_count) {
    kqueue_context_t *context = opaque;
    if (!context || !events || !capacity || !out_count || capacity > (size_t)INT_MAX) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (capacity > context->event_capacity) {
        if (capacity > SIZE_MAX / sizeof(*context->events)) {
            return MAELYS_SYS_ERR_CAPACITY;
        }
        struct kevent *native_events = realloc(
            context->events, capacity * sizeof(*context->events));
        if (!native_events) return MAELYS_SYS_ERR_MEMORY;
        context->events = native_events;
        context->event_capacity = capacity;
    }
    struct timespec timeout;
    struct timespec *timeout_ptr = NULL;
    if (timeout_ms >= 0) {
        timeout.tv_sec = (time_t)(timeout_ms / 1000);
        timeout.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        timeout_ptr = &timeout;
    }
    int count = kevent(
        context->fd, NULL, 0, context->events, (int)capacity, timeout_ptr);
    if (count < 0) {
        return MAELYS_SYS_ERR_OS;
    }
    *out_count = (size_t)count;
    for (int i = 0; i < count; ++i) {
        unsigned flags = 0;
        if (context->events[i].filter == EVFILT_READ) flags |= MAELYS_SYS_EVENT_READ;
        if (context->events[i].filter == EVFILT_WRITE) flags |= MAELYS_SYS_EVENT_WRITE;
        if (context->events[i].flags & EV_EOF) flags |= MAELYS_SYS_EVENT_HUP;
        if (context->events[i].flags & EV_ERROR) flags |= MAELYS_SYS_EVENT_ERROR;
        events[(size_t)i] = (maelys_sys_backend_event_t){
            .watch_id = (uint64_t)(uintptr_t)context->events[i].udata,
            .flags = flags
        };
    }
    return MAELYS_SYS_OK;
}

const maelys_sys_loop_backend_ops_t maelys_sys_kqueue_backend_ops = {
    .name = "kqueue",
    .create = kqueue_create_backend,
    .destroy = kqueue_destroy_backend,
    .add = kqueue_add,
    .modify = kqueue_modify,
    .remove = kqueue_remove,
    .wait = kqueue_wait_backend
};

#endif
