#ifndef MAELYS_SYS_RESULT_H
#define MAELYS_SYS_RESULT_H

#ifdef __cplusplus
extern "C" {
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

