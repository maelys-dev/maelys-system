#ifndef MAELYS_SYS_THREAD_H
#define MAELYS_SYS_THREAD_H

#include <stdint.h>

#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_sys_mutex maelys_sys_mutex_t;
typedef struct maelys_sys_condition maelys_sys_condition_t;
typedef struct maelys_sys_thread maelys_sys_thread_t;
typedef void *(*maelys_sys_thread_fn)(void *context);

maelys_sys_result_t maelys_sys_mutex_create(maelys_sys_mutex_t **out_mutex);
maelys_sys_result_t maelys_sys_mutex_lock(maelys_sys_mutex_t *mutex);
maelys_sys_result_t maelys_sys_mutex_unlock(maelys_sys_mutex_t *mutex);
void maelys_sys_mutex_destroy(maelys_sys_mutex_t *mutex);

maelys_sys_result_t maelys_sys_condition_create(
    maelys_sys_condition_t **out_condition);
/* Requires a finite absolute monotonic deadline; INFINITE is rejected. */
maelys_sys_result_t maelys_sys_condition_wait_until(
    maelys_sys_condition_t *condition,
    maelys_sys_mutex_t *mutex,
    uint64_t deadline_ms);
maelys_sys_result_t maelys_sys_condition_signal(
    maelys_sys_condition_t *condition);
maelys_sys_result_t maelys_sys_condition_broadcast(
    maelys_sys_condition_t *condition);
void maelys_sys_condition_destroy(maelys_sys_condition_t *condition);

maelys_sys_result_t maelys_sys_thread_create(
    const char *name,
    maelys_sys_thread_fn function,
    void *context,
    maelys_sys_thread_t **out_thread);
/* On success, joins, releases, and sets *thread to NULL. */
maelys_sys_result_t maelys_sys_thread_join(
    maelys_sys_thread_t **thread,
    void **out_result);

#ifdef __cplusplus
}
#endif

#endif
