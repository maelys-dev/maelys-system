/*
 * What src/process.c lends to the rest of the library and to the keeper:
 * the start of a program without the wait for its exec report, the
 * descriptor that says a process has ended, and the reading of a raw wait
 * status. Not installed; not ABI.
 */
#ifndef MAELYS_SYS_PROCESS_INTERNAL_H
#define MAELYS_SYS_PROCESS_INTERNAL_H

#include "maelys/sys/process.h"

#include <sys/types.h>

/*
 * Checks the options and starts the program, and returns before its exec
 * is confirmed where that is possible. On Linux *out_report_fd is the
 * read end of the close-on-exec report pipe: it ends (0 bytes) once the
 * exec has succeeded, or carries one int, the errno of the failed exec,
 * after which the child ends with 127 and is the caller's to reap. On
 * macOS posix_spawn confirms the exec itself: *out_report_fd is -1 and
 * the result is final. The result codes are those of
 * maelys_sys_process_spawn before the exec report. *out_pid is set only
 * on OK.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys__process_start(
    const maelys_sys_process_options_t *options,
    pid_t *out_pid,
    int *out_report_fd);

/* The exec report read to its end, as maelys_sys_process_spawn reads it:
 * OK once the program has started; else the child is reaped and the
 * result is the start's. Closes report_fd. */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys__process_await_exec(
    pid_t pid,
    int report_fd);

/* A descriptor readable once the process ends, close-on-exec: a pidfd on
 * Linux, a kqueue on macOS, readable at once for a process that has
 * already ended but is not reaped. -1 with errno. */
int maelys_sys__process_exit_fd_open(pid_t pid);

/* A raw wait status into the public structure. */
void maelys_sys__process_status_fill(maelys_sys_process_status_t *status, int raw);

#endif
