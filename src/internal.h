#ifndef MAELYS_SYS_INTERNAL_H
#define MAELYS_SYS_INTERNAL_H

#include "maelys/sys.h"

#include <pthread.h>

struct maelys_sys_mutex {
    pthread_mutex_t native;
};

struct maelys_sys_condition {
    pthread_cond_t native;
};

struct maelys_sys_thread {
    pthread_t native;
};

#endif

