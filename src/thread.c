#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "src/internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
extern int pthread_cond_timedwait_relative_np(
    pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
#endif

typedef struct thread_start {
    maelys_sys_thread_fn function;
    void *context;
    char name[64];
} thread_start_t;

maelys_sys_result_t maelys_sys_mutex_create(maelys_sys_mutex_t **out_mutex) {
    if (!out_mutex) return MAELYS_SYS_ERR_ARGUMENT;
    *out_mutex = NULL;
    maelys_sys_mutex_t *mutex = malloc(sizeof(*mutex));
    if (!mutex) return MAELYS_SYS_ERR_MEMORY;
    int status = pthread_mutex_init(&mutex->native, NULL);
    if (status != 0) {
        free(mutex);
        errno = status;
        return MAELYS_SYS_ERR_OS;
    }
    *out_mutex = mutex;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_mutex_lock(maelys_sys_mutex_t *mutex) {
    if (!mutex) return MAELYS_SYS_ERR_ARGUMENT;
    int status = pthread_mutex_lock(&mutex->native);
    if (status == 0) return MAELYS_SYS_OK;
    errno = status;
    return MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_mutex_unlock(maelys_sys_mutex_t *mutex) {
    if (!mutex) return MAELYS_SYS_ERR_ARGUMENT;
    int status = pthread_mutex_unlock(&mutex->native);
    if (status == 0) return MAELYS_SYS_OK;
    errno = status;
    return MAELYS_SYS_ERR_OS;
}

void maelys_sys_mutex_destroy(maelys_sys_mutex_t *mutex) {
    if (!mutex) return;
    (void)pthread_mutex_destroy(&mutex->native);
    free(mutex);
}

maelys_sys_result_t maelys_sys_condition_create(
    maelys_sys_condition_t **out_condition) {
    if (!out_condition) return MAELYS_SYS_ERR_ARGUMENT;
    *out_condition = NULL;
    maelys_sys_condition_t *condition = malloc(sizeof(*condition));
    if (!condition) return MAELYS_SYS_ERR_MEMORY;
    int status;
#ifdef __APPLE__
    status = pthread_cond_init(&condition->native, NULL);
#else
    pthread_condattr_t attributes;
    status = pthread_condattr_init(&attributes);
    if (status == 0) {
        status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
        if (status == 0) {
            status = pthread_cond_init(&condition->native, &attributes);
        }
        (void)pthread_condattr_destroy(&attributes);
    }
#endif
    if (status != 0) {
        free(condition);
        errno = status;
        return MAELYS_SYS_ERR_OS;
    }
    *out_condition = condition;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_condition_wait_until(
    maelys_sys_condition_t *condition,
    maelys_sys_mutex_t *mutex,
    uint64_t deadline_ms) {
    if (!condition || !mutex || deadline_ms == MAELYS_SYS_DEADLINE_INFINITE) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    uint64_t remaining = 0;
    maelys_sys_result_t result =
        maelys_sys_deadline_remaining(deadline_ms, &remaining);
    if (result != MAELYS_SYS_OK) return result;
    if (remaining == 0) return MAELYS_SYS_ERR_TIMEOUT;
    int status;
#ifdef __APPLE__
    struct timespec relative = {
        .tv_sec = (time_t)(remaining / 1000u),
        .tv_nsec = (long)((remaining % 1000u) * 1000000u)
    };
    status = pthread_cond_timedwait_relative_np(
        &condition->native, &mutex->native, &relative);
#else
    struct timespec absolute = {
        .tv_sec = (time_t)(deadline_ms / 1000u),
        .tv_nsec = (long)((deadline_ms % 1000u) * 1000000u)
    };
    status = pthread_cond_timedwait(
        &condition->native, &mutex->native, &absolute);
#endif
    if (status == 0) return MAELYS_SYS_OK;
    if (status == ETIMEDOUT) return MAELYS_SYS_ERR_TIMEOUT;
    errno = status;
    return MAELYS_SYS_ERR_OS;
}

static maelys_sys_result_t condition_notify(
    maelys_sys_condition_t *condition,
    int broadcast) {
    if (!condition) return MAELYS_SYS_ERR_ARGUMENT;
    int status = broadcast ? pthread_cond_broadcast(&condition->native) :
        pthread_cond_signal(&condition->native);
    if (status == 0) return MAELYS_SYS_OK;
    errno = status;
    return MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_condition_signal(
    maelys_sys_condition_t *condition) {
    return condition_notify(condition, 0);
}

maelys_sys_result_t maelys_sys_condition_broadcast(
    maelys_sys_condition_t *condition) {
    return condition_notify(condition, 1);
}

void maelys_sys_condition_destroy(maelys_sys_condition_t *condition) {
    if (!condition) return;
    (void)pthread_cond_destroy(&condition->native);
    free(condition);
}

static void *thread_trampoline(void *argument) {
    thread_start_t *start = argument;
#ifdef __APPLE__
    if (start->name[0]) (void)pthread_setname_np(start->name);
#elif defined(__linux__)
    if (start->name[0]) (void)pthread_setname_np(pthread_self(), start->name);
#endif
    maelys_sys_thread_fn function = start->function;
    void *context = start->context;
    free(start);
    return function(context);
}

maelys_sys_result_t maelys_sys_thread_create(
    const char *name,
    maelys_sys_thread_fn function,
    void *context,
    maelys_sys_thread_t **out_thread) {
    if (!function || !out_thread) return MAELYS_SYS_ERR_ARGUMENT;
    *out_thread = NULL;
    maelys_sys_thread_t *thread = malloc(sizeof(*thread));
    thread_start_t *start = calloc(1, sizeof(*start));
    if (!thread || !start) {
        free(thread);
        free(start);
        return MAELYS_SYS_ERR_MEMORY;
    }
    start->function = function;
    start->context = context;
    if (name) {
        size_t length = strlen(name);
        if (length >= sizeof(start->name)) length = sizeof(start->name) - 1;
        memcpy(start->name, name, length);
    }
    int status = pthread_create(&thread->native, NULL, thread_trampoline, start);
    if (status != 0) {
        free(start);
        free(thread);
        errno = status;
        return MAELYS_SYS_ERR_OS;
    }
    *out_thread = thread;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_thread_join(
    maelys_sys_thread_t **thread,
    void **out_result) {
    if (!thread || !*thread) return MAELYS_SYS_ERR_ARGUMENT;
    void *result = NULL;
    int status = pthread_join((*thread)->native, &result);
    if (status != 0) {
        errno = status;
        return MAELYS_SYS_ERR_OS;
    }
    free(*thread);
    *thread = NULL;
    if (out_result) *out_result = result;
    return MAELYS_SYS_OK;
}
