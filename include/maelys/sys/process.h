#ifndef MAELYS_SYS_PROCESS_H
#define MAELYS_SYS_PROCESS_H

/*
 * Processes, in one sentence: start a program with exactly the descriptors
 * named, follow it, stop it within a bound, and tell whether a process
 * number still names the process it did.
 *
 * Promised, alike on Linux and macOS:
 * - In the program, after exec, exactly the targets of the table are open:
 *   not what the caller had open, not what this library opened for itself.
 *   The table is applied as a whole: {3->4, 4->3} exchanges the two, one
 *   source may reach several targets, 2->1 makes standard output the same
 *   file as standard error. A target absent from the table, 0, 1 and 2
 *   included, is not open in the program.
 * - spawn returns once the program has started, never before: an error is
 *   this start's, never the program's afterwards.
 * - The process number is reserved from spawn to release: nothing is
 *   reaped before, so a signal cannot reach a stranger that inherited the
 *   number. The contract has two sides: the caller does not reap blindly
 *   (no wait(-1), no waitpid(-1)) and leaves SIGCHLD neither SIG_IGN nor
 *   SA_NOCLDWAIT, under which the kernel reaps by itself.
 * - No wait is unbounded: wait takes a deadline, terminate is bounded by
 *   its two durations, release never blocks.
 * - Nothing here keeps global state, starts a thread, installs a signal
 *   handler or calls wait(-1). On the host that forks, between fork and
 *   exec only async-signal-safe calls run and nothing is allocated, so
 *   spawn is legal from a process with other threads.
 *
 * Not here: searching PATH, reading a shebang, judging whether a file may
 * be run, a terminal, confinement, pipes and their buffers. The caller
 * opens its pipes and names their ends in the table.
 *
 * A handle belongs to one thread, except signal and pid, which any thread
 * may call while the handle lives. release requires that no thread is
 * still inside either, as destroying a wakeup does.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_sys_process maelys_sys_process_t;

/* One entry of the table: source is open in the caller, target is the
 * number the program sees it as. Any target, 0 included. */
typedef struct maelys_sys_process_fd {
    int source;
    int target;
} maelys_sys_process_fd_t;

/* The table is staged on a fixed array before anything is applied, with
 * nothing allocated: that is what bounds it. */
#define MAELYS_SYS_PROCESS_MAX_FDS 32u

typedef enum maelys_sys_process_flags {
    /* The program leads a new session (setsid). */
    MAELYS_SYS_PROCESS_NEW_SESSION = 1u << 0,
    /* The program leads a new process group; exclusive of NEW_SESSION,
     * which already does that. */
    MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP = 1u << 1
} maelys_sys_process_flags_t;

/* Fields fixed within ABI 1, like every public structure the library
 * reads: a new option is a new structure and a new function. */
typedef struct maelys_sys_process_options {
    const char *path;          /* absolute; no search of PATH */
    char *const *argv;         /* argv[0] is the caller's to choose */
    char *const *envp;         /* NULL: the caller's environment; else exactly this */
    const char *cwd;           /* NULL: unchanged */
    const maelys_sys_process_fd_t *fds;
    size_t fd_count;           /* at most MAELYS_SYS_PROCESS_MAX_FDS */
    unsigned flags;
} maelys_sys_process_options_t;

/*
 * Starts the program. path, argv and envp are borrowed for the call only
 * and copied by nothing: exec reads them. ERR_ARGUMENT before any process
 * exists: a relative path, a negative target, two entries for one target,
 * a source that is not open, more than MAX_FDS entries, both exclusive
 * flags. ERR_NOT_FOUND when the path names nothing (ENOENT). Any other
 * refusal of the start, a file the caller may not run included, is ERR_OS
 * with errno; so is a failure to create the process or the descriptor
 * that reports its end, after which no process is left. On ERR_* nothing
 * is started and *out_process is NULL. On macOS the process a refused
 * start aborted stays visible to a wait for a few tens of microseconds,
 * measured, and leaves no zombie.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_process_spawn(
    const maelys_sys_process_options_t *options,
    maelys_sys_process_t **out_process);

/* -1 for NULL. */
pid_t maelys_sys_process_pid(const maelys_sys_process_t *process);

/* Filled by wait and terminate. Fields fixed within ABI 1. */
typedef struct maelys_sys_process_status {
    int exited;       /* ended by exit: exit_code holds the code */
    int exit_code;
    int signaled;     /* ended by a signal: term_signal holds its number */
    int term_signal;
} maelys_sys_process_status_t;

/*
 * Waits for the program to end, no later than the absolute monotonic
 * deadline; a deadline already past is one look without waiting, and
 * MAELYS_SYS_DEADLINE_INFINITE is ERR_ARGUMENT. OK with *out_status once
 * the program is reaped, which only this call and terminate do, always by
 * number and never -1. ERR_TIMEOUT when it still runs. ERR_STATE once it
 * has been reaped. The wait sleeps on the descriptor exit_fd lends: no
 * polling.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_process_wait(
    maelys_sys_process_t *process,
    uint64_t deadline_ms,
    maelys_sys_process_status_t *out_status);

/*
 * Sends signo to the program, from any thread. ERR_STATE once it has been
 * reaped, ERR_ARGUMENT for a signal the host does not know; on ERR_OS
 * errno identifies kill(2). A program that has ended and is not reaped
 * yet takes the signal silently.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_process_signal(
    maelys_sys_process_t *process,
    int signo);

/*
 * A descriptor readable once the program has ended, for a loop to watch;
 * -1 for NULL. Borrowed from the handle, closed by release: unwatch it
 * first, as loop.h requires. Each handle holds one such descriptor on both
 * hosts, counted against the descriptor limit, 256 by default on macOS.
 */
int maelys_sys_process_exit_fd(const maelys_sys_process_t *process);

/*
 * Stops the program within grace_ms + force_ms: SIGTERM, a wait of
 * grace_ms, then SIGKILL and a wait of force_ms. OK with *out_status once
 * reaped, at once for a program already ended, with its own status for one
 * already reaped. ERR_TIMEOUT when it outlives SIGKILL, in an
 * uninterruptible sleep: the handle stays valid and the caller decides.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_process_terminate(
    maelys_sys_process_t *process,
    uint64_t grace_ms,
    uint64_t force_ms,
    maelys_sys_process_status_t *out_status);

/*
 * Releases the handle and its descriptor, and sets *process to NULL; never
 * blocks. A program that has ended is reaped on the way, its status lost:
 * wait first when it matters. ERR_STATE says the program still runs: the
 * handle is released all the same, its number is no longer reserved, and
 * what the program becomes is the caller's. NULL and an already-NULL
 * handle are successes that do nothing.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_process_release(
    maelys_sys_process_t **process);

/*
 * The identity of any process on the host, launched here or not: its
 * number, when it started and when the system booted, as the host counts
 * them. The two numbers are opaque: compared, never interpreted, and not
 * comparable across hosts. Fields fixed within ABI 1.
 */
typedef struct maelys_sys_process_identity {
    pid_t pid;
    uint64_t birth;
    uint64_t boot;
} maelys_sys_process_identity_t;

/*
 * ERR_NOT_FOUND when no process has the number; ERR_OS with errno when the
 * host refuses to say, as it may of another user's process. A process
 * that has ended and is not reaped yet still has an identity on Linux and
 * none on macOS, measured: an identity proves nothing about the process
 * running, only about which process a number names.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_process_identify(
    pid_t pid,
    maelys_sys_process_identity_t *out_identity);

/*
 * *out_alive is 1 when a process with this number, this birth and this
 * boot exists, 0 otherwise: never a namesake that took the number later.
 * ERR_OS when the host refuses to say.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_process_alive(
    const maelys_sys_process_identity_t *identity,
    int *out_alive);

/*
 * Like exit_fd, for a process this handle did not start: a descriptor,
 * owned by the caller and close-on-exec, readable once the process has
 * ended. The identity is checked before and after the registration;
 * ERR_NOT_FOUND, and no descriptor, when it does not match at either
 * point, so the descriptor never follows a namesake.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_process_exit_fd_open(
    const maelys_sys_process_identity_t *identity,
    int *out_fd);

#ifdef __cplusplus
}
#endif

#endif
