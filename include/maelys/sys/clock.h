#ifndef MAELYS_SYS_CLOCK_H
#define MAELYS_SYS_CLOCK_H

#include <stdint.h>

#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Milliseconds. monotonic_ms reads CLOCK_MONOTONIC: it never goes back, its
 * origin is arbitrary, and every deadline of this library is one of its
 * values. wall_ms reads CLOCK_REALTIME, which an administrator or a time
 * daemon moves: a date to show or to store, never a deadline.
 *
 * The hosts disagree about a machine that sleeps. On macOS the monotonic
 * clock goes on during sleep, measured. Linux documents that
 * CLOCK_MONOTONIC leaves suspend out, which nothing here observes. So a
 * deadline bounds time on the wall on macOS and time the system was running
 * on Linux; a caller that must bound one of the two on both hosts measures
 * it itself.
 */
#define MAELYS_SYS_DEADLINE_INFINITE UINT64_MAX

MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_monotonic_ms(uint64_t *out_value);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_wall_ms(uint64_t *out_value);
/*
 * Constructs a finite absolute monotonic deadline. The INFINITE sentinel is
 * not a duration and is rejected with ERR_ARGUMENT.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_deadline_after(
    uint64_t timeout_ms,
    uint64_t *out_deadline_ms);
/* INFINITE is accepted and remains INFINITE/not expired. */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_deadline_remaining(
    uint64_t deadline_ms,
    uint64_t *out_remaining_ms);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_deadline_expired(
    uint64_t deadline_ms,
    int *out_expired);

#ifdef __cplusplus
}
#endif

#endif
