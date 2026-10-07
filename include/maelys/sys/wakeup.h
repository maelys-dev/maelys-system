#ifndef MAELYS_SYS_WAKEUP_H
#define MAELYS_SYS_WAKEUP_H

#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * "Look again", from one thread to the loop of another. Any thread may
 * signal; the thread that watches the descriptor consumes. Signals are not
 * counted: any number of them before a consume make the descriptor readable
 * once, and it stays readable until consumed.
 *
 * Between threads, never from a signal handler. On macOS signal and consume
 * take a mutex, and no mutex is async-signal-safe: a handler that
 * interrupts a thread inside either of them waits for a lock that thread
 * can no longer release. Linux happens to make one write(2) to an eventfd,
 * and nothing is promised there either. A handler that has to wake a loop
 * writes one byte to a non-blocking pipe of the caller's own, whose other
 * end the loop watches.
 */
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

