#ifndef MAELYS_SYS_RESULT_H
#define MAELYS_SYS_RESULT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Define MAELYS_SYS_STRICT_RESULTS to mark every result-returning function
 * warn_unused_result: a result that is not read becomes a warning, and a
 * (void) cast states that ignoring it is intended. Clang honors the cast;
 * GCC warns through it, so the option suits Clang builds. Off by default
 * so a consumer's build does not change when it re-pins.
 */
#if defined(MAELYS_SYS_STRICT_RESULTS) && (defined(__GNUC__) || defined(__clang__))
#define MAELYS_SYS_NODISCARD __attribute__((warn_unused_result))
#else
#define MAELYS_SYS_NODISCARD
#endif

typedef enum maelys_sys_result {
    MAELYS_SYS_OK = 0,
    MAELYS_SYS_ERR_ARGUMENT,
    MAELYS_SYS_ERR_MEMORY,
    MAELYS_SYS_ERR_OS,
    MAELYS_SYS_ERR_TIMEOUT,
    MAELYS_SYS_ERR_CLOSED,
    MAELYS_SYS_ERR_STATE,
    MAELYS_SYS_ERR_NOT_FOUND,
    MAELYS_SYS_ERR_CAPACITY,
    MAELYS_SYS_ERR_UNSUPPORTED
} maelys_sys_result_t;

const char *maelys_sys_result_string(maelys_sys_result_t result);

#ifdef __cplusplus
}
#endif

#endif

