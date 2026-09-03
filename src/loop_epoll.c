#ifdef __linux__

#include "src/loop_backend.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct epoll_context {
    int fd;
    struct epoll_event *events;
    size_t event_capacity;
} epoll_context_t;

static uint32_t epoll_interests(unsigned interests) {
    uint32_t events = EPOLLRDHUP;
    if (interests & MAELYS_SYS_INTEREST_READ) events |= EPOLLIN;
    if (interests & MAELYS_SYS_INTEREST_WRITE) events |= EPOLLOUT;
    return events;
}

static maelys_sys_result_t epoll_create_backend(void **out_context) {
    if (!out_context) return MAELYS_SYS_ERR_ARGUMENT;
    *out_context = NULL;
    epoll_context_t *context = calloc(1, sizeof(*context));
    if (!context) return MAELYS_SYS_ERR_MEMORY;
    context->fd = epoll_create1(EPOLL_CLOEXEC);
    if (context->fd < 0) {
        free(context);
        return MAELYS_SYS_ERR_OS;
    }
    *out_context = context;
    return MAELYS_SYS_OK;
}

static void epoll_destroy_backend(void *opaque) {
    epoll_context_t *context = opaque;
    if (!context) return;
    if (context->fd >= 0) close(context->fd);
    free(context->events);
    free(context);
}

static maelys_sys_result_t epoll_control(
    epoll_context_t *context, int operation, int fd,
    unsigned interests, uint64_t watch_id) {
    struct epoll_event event = {
        .events = epoll_interests(interests),
        .data.u64 = watch_id
    };
    if (epoll_ctl(context->fd, operation, fd,
        operation == EPOLL_CTL_DEL ? NULL : &event) != 0) {
        return errno == ENOENT ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    }
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t epoll_add(
    void *opaque, int fd, unsigned interests, uint64_t watch_id) {
    return epoll_control(opaque, EPOLL_CTL_ADD, fd, interests, watch_id);
}

static maelys_sys_result_t epoll_modify(
    void *opaque, int fd, unsigned old_interests,
    unsigned new_interests, uint64_t watch_id) {
    (void)old_interests;
    return epoll_control(opaque, EPOLL_CTL_MOD, fd, new_interests, watch_id);
}

static maelys_sys_result_t epoll_remove(
    void *opaque, int fd, unsigned interests, uint64_t watch_id) {
    (void)watch_id;
    return epoll_control(opaque, EPOLL_CTL_DEL, fd, interests, 0);
}

static unsigned epoll_flags(uint32_t events) {
    unsigned flags = 0;
    if (events & EPOLLIN) flags |= MAELYS_SYS_EVENT_READ;
    if (events & EPOLLOUT) flags |= MAELYS_SYS_EVENT_WRITE;
    if (events & (EPOLLHUP | EPOLLRDHUP)) flags |= MAELYS_SYS_EVENT_HUP;
    if (events & EPOLLERR) flags |= MAELYS_SYS_EVENT_ERROR;
    return flags;
}

static maelys_sys_result_t epoll_wait_backend(
    void *opaque, int timeout_ms,
    maelys_sys_backend_event_t *events, size_t capacity,
    size_t *out_count) {
    epoll_context_t *context = opaque;
    if (!context || !events || !capacity || !out_count || capacity > (size_t)INT_MAX) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (capacity > context->event_capacity) {
        if (capacity > SIZE_MAX / sizeof(*context->events)) {
            return MAELYS_SYS_ERR_CAPACITY;
        }
        struct epoll_event *native_events = realloc(
            context->events, capacity * sizeof(*context->events));
        if (!native_events) return MAELYS_SYS_ERR_MEMORY;
        context->events = native_events;
        context->event_capacity = capacity;
    }
    int count = epoll_wait(
        context->fd, context->events, (int)capacity, timeout_ms);
    if (count < 0) {
        return MAELYS_SYS_ERR_OS;
    }
    *out_count = (size_t)count;
    for (int i = 0; i < count; ++i) {
        events[(size_t)i] = (maelys_sys_backend_event_t){
            .watch_id = context->events[i].data.u64,
            .flags = epoll_flags(context->events[i].events)
        };
    }
    return MAELYS_SYS_OK;
}

const maelys_sys_loop_backend_ops_t maelys_sys_epoll_backend_ops = {
    .name = "epoll",
    .create = epoll_create_backend,
    .destroy = epoll_destroy_backend,
    .add = epoll_add,
    .modify = epoll_modify,
    .remove = epoll_remove,
    .wait = epoll_wait_backend
};

#endif
