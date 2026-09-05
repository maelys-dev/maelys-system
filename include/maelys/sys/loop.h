#ifndef MAELYS_SYS_LOOP_H
#define MAELYS_SYS_LOOP_H

#include <stddef.h>
#include <stdint.h>

#include "maelys/sys/clock.h"
#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_sys_loop maelys_sys_loop_t;
typedef uint64_t maelys_sys_watch_t;
typedef uint64_t maelys_sys_timer_t;
typedef uint64_t maelys_sys_token_t;

/* AUTO is the native backend of the host (epoll, kqueue), else poll. */
typedef enum maelys_sys_loop_backend {
    MAELYS_SYS_LOOP_AUTO = 0,
    MAELYS_SYS_LOOP_POLL,
    MAELYS_SYS_LOOP_EPOLL,
    MAELYS_SYS_LOOP_KQUEUE
} maelys_sys_loop_backend_t;

typedef enum maelys_sys_interest {
    MAELYS_SYS_INTEREST_READ = 1u << 0,
    MAELYS_SYS_INTEREST_WRITE = 1u << 1
} maelys_sys_interest_t;

typedef enum maelys_sys_event_flags {
    MAELYS_SYS_EVENT_READ = 1u << 0,
    MAELYS_SYS_EVENT_WRITE = 1u << 1,
    MAELYS_SYS_EVENT_HUP = 1u << 2,
    MAELYS_SYS_EVENT_ERROR = 1u << 3,
    MAELYS_SYS_EVENT_TIMER = 1u << 4,
    MAELYS_SYS_EVENT_WAKE = 1u << 5
} maelys_sys_event_flags_t;

typedef struct maelys_sys_event {
    maelys_sys_token_t token;
    unsigned flags;
} maelys_sys_event_t;

typedef enum maelys_sys_step_result {
    MAELYS_SYS_STEP_PROGRESS = 0,
    MAELYS_SYS_STEP_TIMEOUT,
    MAELYS_SYS_STEP_STOPPED
} maelys_sys_step_result_t;

int maelys_sys_loop_backend_available(maelys_sys_loop_backend_t backend);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_create(
    maelys_sys_loop_backend_t backend,
    maelys_sys_loop_t **out_loop);
const char *maelys_sys_loop_backend_name(const maelys_sys_loop_t *loop);

/*
 * The loop borrows fd, a stream descriptor: a socket, pipe, FIFO, terminal
 * or device. A regular file is refused by epoll and always ready
 * elsewhere; it is not a watchable object. The owner must unwatch before
 * closing the descriptor. Should it close first, unwatch still releases
 * the registration and reports OK; on Linux a dup of that descriptor
 * keeps the epoll registration, and its events, alive until the dup
 * closes.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_watch_fd(
    maelys_sys_loop_t *loop,
    int fd,
    unsigned interests,
    maelys_sys_token_t token,
    maelys_sys_watch_t *out_watch);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_modify(
    maelys_sys_loop_t *loop,
    maelys_sys_watch_t watch,
    unsigned interests);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_unwatch(
    maelys_sys_loop_t *loop,
    maelys_sys_watch_t watch);

/*
 * Timers are one-shot and use CLOCK_MONOTONIC absolute milliseconds.
 * MAELYS_SYS_DEADLINE_INFINITE is not a timer deadline and is rejected.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_timer_add(
    maelys_sys_loop_t *loop,
    uint64_t deadline_ms,
    maelys_sys_token_t token,
    maelys_sys_timer_t *out_timer);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_timer_cancel(
    maelys_sys_loop_t *loop,
    maelys_sys_timer_t timer);

/*
 * Events. READ and WRITE follow the requested interests. HUP is reported
 * on every backend when the watch includes READ and the peer closed or
 * shut its writing side; for a watch on WRITE alone the hosts disagree
 * (Linux reports it, macOS mostly not) and nothing is promised. ERROR is
 * an indication, not a contract: a reset arrives as READ|HUP|ERROR on
 * Linux and on kqueue, as READ|HUP on the poll backend of macOS; the read
 * or write that follows carries the error either way. A watch yields at
 * most one event per step. A wake that does not fit in the caller's array
 * stays pending for the next step.
 *
 * Registration, timers, step and destroy are owner-thread-only. wake and stop
 * are the only cross-thread operations. The loop never invokes callbacks.
 * step accepts MAELYS_SYS_DEADLINE_INFINITE and then waits for an event, timer,
 * wake or stop without imposing its own deadline.
 *
 * Every backend reports the same events. A watch yields at most one event per
 * step, whose flags combine every ready direction. HUP is set when the peer
 * closed its writing side or the connection ended; READ usually accompanies
 * it and a read then returns the remaining bytes and then end of stream.
 * Readiness is level-triggered: what does not fit in events is reported by a
 * later step. A wake that does not fit is left pending rather than consumed,
 * so it is never lost.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_step(
    maelys_sys_loop_t *loop,
    uint64_t deadline_ms,
    maelys_sys_event_t *events,
    size_t event_capacity,
    size_t *out_event_count,
    maelys_sys_step_result_t *out_step_result);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_wake(maelys_sys_loop_t *loop);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_stop(maelys_sys_loop_t *loop);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_loop_destroy(maelys_sys_loop_t **loop);

#ifdef __cplusplus
}
#endif

#endif
