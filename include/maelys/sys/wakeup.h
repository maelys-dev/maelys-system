#ifndef MAELYS_SYS_WAKEUP_H
#define MAELYS_SYS_WAKEUP_H

#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_sys_wakeup maelys_sys_wakeup_t;

MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_wakeup_create(
    maelys_sys_wakeup_t **out_wakeup);
/* Borrowed readable descriptor, valid until destroy, never closed by the
 * caller; -1 for NULL. Watch it for READ, then consume. */
int maelys_sys_wakeup_fd(const maelys_sys_wakeup_t *wakeup);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_wakeup_signal(maelys_sys_wakeup_t *wakeup);
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_wakeup_consume(maelys_sys_wakeup_t *wakeup);
void maelys_sys_wakeup_destroy(maelys_sys_wakeup_t *wakeup);

#ifdef __cplusplus
}
#endif

#endif

