#include "maelys/sys/clock.h"

#include <errno.h>
#include <time.h>

static maelys_sys_result_t clock_ms(clockid_t id, uint64_t *out_value) {
    if (!out_value) return MAELYS_SYS_ERR_ARGUMENT;
    struct timespec value;
    if (clock_gettime(id, &value) != 0) return MAELYS_SYS_ERR_OS;
    if (value.tv_sec < 0 || value.tv_nsec < 0) {
        errno = EOVERFLOW;
        return MAELYS_SYS_ERR_OS;
    }
    uint64_t seconds = (uint64_t)value.tv_sec;
    if (seconds > UINT64_MAX / 1000u) {
        errno = EOVERFLOW;
        return MAELYS_SYS_ERR_OS;
    }
    *out_value = seconds * 1000u + (uint64_t)value.tv_nsec / 1000000u;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_monotonic_ms(uint64_t *out_value) {
    return clock_ms(CLOCK_MONOTONIC, out_value);
}

maelys_sys_result_t maelys_sys_wall_ms(uint64_t *out_value) {
    return clock_ms(CLOCK_REALTIME, out_value);
}

maelys_sys_result_t maelys_sys_deadline_after(
    uint64_t timeout_ms,
    uint64_t *out_deadline_ms) {
    if (!out_deadline_ms || timeout_ms == MAELYS_SYS_DEADLINE_INFINITE) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    uint64_t now = 0;
    maelys_sys_result_t result = maelys_sys_monotonic_ms(&now);
    if (result != MAELYS_SYS_OK) return result;
    if (timeout_ms > UINT64_MAX - now) {
        errno = EOVERFLOW;
        return MAELYS_SYS_ERR_OS;
    }
    *out_deadline_ms = now + timeout_ms;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_deadline_remaining(
    uint64_t deadline_ms,
    uint64_t *out_remaining_ms) {
    if (!out_remaining_ms) return MAELYS_SYS_ERR_ARGUMENT;
    if (deadline_ms == MAELYS_SYS_DEADLINE_INFINITE) {
        *out_remaining_ms = MAELYS_SYS_DEADLINE_INFINITE;
        return MAELYS_SYS_OK;
    }
    uint64_t now = 0;
    maelys_sys_result_t result = maelys_sys_monotonic_ms(&now);
    if (result != MAELYS_SYS_OK) return result;
    *out_remaining_ms = now >= deadline_ms ? 0 : deadline_ms - now;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_deadline_expired(
    uint64_t deadline_ms,
    int *out_expired) {
    if (!out_expired) return MAELYS_SYS_ERR_ARGUMENT;
    uint64_t remaining = 0;
    maelys_sys_result_t result =
        maelys_sys_deadline_remaining(deadline_ms, &remaining);
    if (result != MAELYS_SYS_OK) return result;
    *out_expired = remaining == 0;
    return MAELYS_SYS_OK;
}

