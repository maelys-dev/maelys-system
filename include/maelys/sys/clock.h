#ifndef MAELYS_SYS_CLOCK_H
#define MAELYS_SYS_CLOCK_H

#include <stdint.h>

#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAELYS_SYS_DEADLINE_INFINITE UINT64_MAX

maelys_sys_result_t maelys_sys_monotonic_ms(uint64_t *out_value);
maelys_sys_result_t maelys_sys_wall_ms(uint64_t *out_value);
/*
 * Constructs a finite absolute monotonic deadline. The INFINITE sentinel is
 * not a duration and is rejected with ERR_ARGUMENT.
 */
maelys_sys_result_t maelys_sys_deadline_after(
    uint64_t timeout_ms,
    uint64_t *out_deadline_ms);
/* INFINITE is accepted and remains INFINITE/not expired. */
maelys_sys_result_t maelys_sys_deadline_remaining(
    uint64_t deadline_ms,
    uint64_t *out_remaining_ms);
maelys_sys_result_t maelys_sys_deadline_expired(
    uint64_t deadline_ms,
    int *out_expired);

#ifdef __cplusplus
}
#endif

#endif
