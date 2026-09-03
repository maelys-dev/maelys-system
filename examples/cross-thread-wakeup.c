#define _POSIX_C_SOURCE 200809L

#include "maelys/sys.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct worker_context {
    maelys_sys_loop_t *loop;
    maelys_sys_mutex_t *mutex;
    unsigned completed;
} worker_context_t;

static void *worker_main(void *opaque) {
    worker_context_t *context = opaque;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
    if (maelys_sys_mutex_lock(context->mutex) != MAELYS_SYS_OK) {
        return (void *)(uintptr_t)1;
    }
    context->completed = 1;
    if (maelys_sys_mutex_unlock(context->mutex) != MAELYS_SYS_OK) {
        return (void *)(uintptr_t)2;
    }
    return (void *)(uintptr_t)maelys_sys_loop_wake(context->loop);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("usage: cross-thread-wakeup");
        return 0;
    }
    if (argc != 1) return 2;
    maelys_sys_loop_t *loop = NULL;
    maelys_sys_mutex_t *mutex = NULL;
    maelys_sys_thread_t *thread = NULL;
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) != MAELYS_SYS_OK ||
        maelys_sys_mutex_create(&mutex) != MAELYS_SYS_OK) return 1;
    worker_context_t context = {.loop = loop, .mutex = mutex, .completed = 0};
    if (maelys_sys_thread_create("wakeup-example", worker_main,
            &context, &thread) != MAELYS_SYS_OK) return 1;

    unsigned completed = 0;
    while (!completed) {
        maelys_sys_event_t events[2];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        if (maelys_sys_loop_step(loop, MAELYS_SYS_DEADLINE_INFINITE,
                events, 2, &count, &step) != MAELYS_SYS_OK) return 1;
        if (maelys_sys_mutex_lock(mutex) != MAELYS_SYS_OK) return 1;
        completed = context.completed;
        if (maelys_sys_mutex_unlock(mutex) != MAELYS_SYS_OK) return 1;
    }
    void *worker_result = NULL;
    if (maelys_sys_thread_join(&thread, &worker_result) != MAELYS_SYS_OK ||
        (uintptr_t)worker_result != MAELYS_SYS_OK) return 1;
    puts("worker complete");
    maelys_sys_mutex_destroy(mutex);
    return maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK ? 0 : 1;
}
