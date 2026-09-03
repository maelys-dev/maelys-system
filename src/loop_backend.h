#ifndef MAELYS_SYS_LOOP_BACKEND_H
#define MAELYS_SYS_LOOP_BACKEND_H

#include "maelys/sys/loop.h"

typedef struct maelys_sys_backend_event {
    uint64_t watch_id;
    unsigned flags;
} maelys_sys_backend_event_t;

typedef struct maelys_sys_loop_backend_ops {
    const char *name;
    maelys_sys_result_t (*create)(void **out_context);
    void (*destroy)(void *context);
    maelys_sys_result_t (*add)(
        void *context, int fd, unsigned interests, uint64_t watch_id);
    maelys_sys_result_t (*modify)(
        void *context, int fd, unsigned old_interests,
        unsigned new_interests, uint64_t watch_id);
    maelys_sys_result_t (*remove)(
        void *context, int fd, unsigned interests, uint64_t watch_id);
    maelys_sys_result_t (*wait)(
        void *context, int timeout_ms,
        maelys_sys_backend_event_t *events, size_t capacity,
        size_t *out_count);
} maelys_sys_loop_backend_ops_t;

extern const maelys_sys_loop_backend_ops_t maelys_sys_poll_backend_ops;
#ifdef __linux__
extern const maelys_sys_loop_backend_ops_t maelys_sys_epoll_backend_ops;
#endif
#ifdef __APPLE__
extern const maelys_sys_loop_backend_ops_t maelys_sys_kqueue_backend_ops;
#endif

#endif

