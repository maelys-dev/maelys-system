#ifndef MAELYS_SYS_OWNED_H
#define MAELYS_SYS_OWNED_H

/*
 * Owned process groups, in one sentence: start a program as the leader of
 * a process group that a keeper, outside the group, signals as a whole and
 * kills when the owner dies, SIGKILL of the host included.
 *
 * The arrangement:
 *
 *   host ──start──▶ keeper (its own group, a child of the host)
 *                     └──start──▶ program = leader of the group X (pid X)
 *                                    └── descendants that stay in X
 *
 * The keeper is an executable installed with the library, under
 * libexec/maelys-sys/keeper of the prefix; the host names its absolute
 * path. It alone signals the group. It observes the end of the leader
 * without reaping it, so that the number X stays taken and no stranger
 * can be reached by a signal to -X, and it reaps the leader only after
 * its last signal. The host never signals the group, and never signals
 * the keeper by its number once owned_spawn has returned: it sends
 * requests on a control channel and reads what the keeper reports.
 *
 * Promised, alike on Linux and macOS:
 * - The owner is the calling process, by identity (number, birth, boot).
 *   The owner is lost when that identity ends, or when every copy of a
 *   life pipe the host holds is closed, whichever comes first. The keeper
 *   then sends SIGKILL to the group at once, without grace. An exec of the
 *   host does not change its identity; it closes the life pipe, which
 *   CLOEXEC makes a loss unless a copy survives in a child.
 * - The program starts exactly as maelys_sys_process_spawn starts one: the
 *   table of descriptors, nothing else open, no signal blocked, every
 *   signal at its default action, as the leader of a new process group.
 * - Covered: the members that stay in X and that the keeper may signal. A
 *   member that leaves by setsid or setpgid, or whose credentials refuse
 *   the signal, is outside the guarantee; kill(-X) succeeds when one
 *   member took the signal, so a refused member can hide behind a success.
 *   The end of each member is not observed one by one: the group is
 *   observed as a whole, with a signal 0, which acts on nobody.
 * - No wait is unbounded, in the host or in the keeper: the host waits in
 *   poll under a deadline, the keeper in its loop under its own; the
 *   system calls themselves are bounded by nothing this library controls.
 * - Nothing here keeps global state, starts a thread or installs a signal
 *   handler in the host. What the keeper does in its own process is its
 *   own: it blocks every signal but KILL and STOP and reads them in its
 *   loop; a TERM, INT or HUP to the keeper is a loss of the owner.
 *
 * The host must, from the call to owned_spawn until owned_release has
 * reaped the keeper: leave SIGCHLD neither SIG_IGN nor SA_NOCLDWAIT, and
 * reap the keeper nowhere else (no wait(-1), no waitpid(-1), no handler
 * that reaps). owned_spawn reads the disposition on entry and refuses
 * ERR_STATE; that catches the common violation, not every one. Without
 * it the keeper could be reaped between its creation and the opening of
 * its end descriptor, which would then follow a stranger.
 *
 * A difference with process_spawn, first: an error of owned_spawn may
 * leave a non-NULL handle, ERR_TIMEOUT above all, which the caller follows
 * and releases like any other. ERR_TIMEOUT says the host did not confirm
 * the start within its deadline, not that the program did not start, nor
 * that it has stopped: the handle says what became of it.
 *
 * Not here: the transfer of a group to another owner (TRANSFER is
 * refused), PATH, shebangs, confinement, pipes. The caller opens its
 * pipes and names their ends in the program's table, 29 entries at most:
 * the keeper takes three.
 *
 * A handle belongs to one thread, except owned_pid and the two
 * descriptors, which any thread may read while the handle lives;
 * owned_result reads what poll writes and belongs to that thread.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "maelys/sys/process.h"
#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_sys_owned maelys_sys_owned_t;

/* Three entries of a table the keeper takes for itself. */
#define MAELYS_SYS_OWNED_MAX_FDS 29u

typedef enum maelys_sys_owned_flags {
    /* When the leader ends: SIGKILL to the group at once, then the end of
     * the group. Without it the protection lasts until owned_stop or the
     * loss of the owner, the leader's number held all along. */
    MAELYS_SYS_OWNED_END_WITH_LEADER = 1u << 0
} maelys_sys_owned_flags_t;

/* Fields fixed within ABI 1, like every public structure the library
 * reads: a new option is a new structure and a new function. */
typedef struct maelys_sys_owned_options {
    const char *keeper_path;                     /* absolute; no search */
    const maelys_sys_process_options_t *program; /* MAX_FDS entries; group and session flags refused */
    unsigned flags;
    uint64_t start_deadline_ms;  /* absolute, monotone: the bound of owned_spawn, handed to the keeper */
    uint64_t heartbeat_ms;       /* the keeper beats this often; 0: 1000 */
    uint64_t observe_ms;         /* after the last SIGKILL: reaping and observation before UNCERTAIN; 0: 2000 */
    uint64_t linger_ms;          /* how long the keeper keeps observing after UNCERTAIN; 0: 60000 */
} maelys_sys_owned_options_t;

typedef enum maelys_sys_owned_phase {
    MAELYS_SYS_OWNED_STARTING,        /* owned_spawn has not returned OK yet */
    MAELYS_SYS_OWNED_RUNNING,         /* leader alive, protection active */
    MAELYS_SYS_OWNED_LEADER_ENDED,    /* status known, leader held unreaped, protection active */
    MAELYS_SYS_OWNED_STOPPING,        /* SIGTERM sent to the group, grace running */
    MAELYS_SYS_OWNED_RECLAIMING,      /* last SIGKILL sent, the leader being reaped */
    MAELYS_SYS_OWNED_OBSERVING,       /* leader reaped, the group observed with signal 0 */
    MAELYS_SYS_OWNED_UNCERTAIN,       /* members present or signal refused after observe_ms; still observed */
    MAELYS_SYS_OWNED_ENDED,           /* final: no live member observable */
    MAELYS_SYS_OWNED_UNCERTAIN_FINAL, /* final: linger_ms elapsed, the keeper has left; cleanup abandoned */
    MAELYS_SYS_OWNED_KEEPER_LOST,     /* final: keeper dead or gone without a last word: protection lost */
    MAELYS_SYS_OWNED_START_FAILED     /* the start was interrupted; the keeper cleans up if it lives */
} maelys_sys_owned_phase_t;

/* Filled by owned_poll and owned_wait. Fields fixed within ABI 1. */
typedef struct maelys_sys_owned_state {
    maelys_sys_owned_phase_t phase;
    int leader_ended;                   /* leader holds its status */
    maelys_sys_process_status_t leader; /* the program's, never the keeper's */
    int leader_unreaped;         /* after SIGKILL the leader still cannot be reaped */
    int last_signal_refused;     /* kill(-X): EPERM, no member signaled */
    int members_present;         /* kill(-X, 0) == 0: at least one member, undiagnosed */
    int keeper_unresponsive;     /* no heartbeat within two periods */
    int keeper_failure;          /* errno of an internal failure of the keeper; 0: none */
    int keeper_reaped;           /* the keeper has been reaped: release can succeed */
    int keeper_reaped_elsewhere; /* ECHILD: reaped outside release, against the contract */
    int truncated_final;         /* a partial record followed the keeper's death */
    int started_after_timeout;   /* STARTED read after ERR_TIMEOUT: START_FAILED became RUNNING */
    int state_from_exit_code;    /* the final phase was taken from the keeper's exit code */
    int want_write;              /* a command waits for owned_fd to be writable */
    uint64_t next_deadline_ms;   /* when the host should poll again (heartbeat check); INFINITE: never */
} maelys_sys_owned_state_t;

/*
 * Starts the keeper, then the program through it, and returns once the
 * keeper has written that the program started, or at start_deadline_ms.
 * ERR_ARGUMENT before anything exists: NULL or relative keeper_path, a
 * program with group or session flags or more than MAX_FDS entries, and
 * what process_spawn refuses of a table. ERR_STATE when SIGCHLD is
 * SIG_IGN or SA_NOCLDWAIT in the caller. ERR_UNSUPPORTED when the keeper
 * is missing (ENOENT) or speaks another protocol: nothing is started in
 * its place. ERR_NOT_FOUND and ERR_OS for the program as process_spawn
 * gives them. ERR_TIMEOUT when the start is not confirmed in time; then
 * *out_owned may be non-NULL, in phase START_FAILED, and the keeper, if
 * it lives, stops what exists. ERR_CLOSED when the keeper dies before
 * the start is confirmed: gone before it was handed the program,
 * nothing is left and *out_owned is NULL; gone after, *out_owned is
 * non-NULL in phase KEEPER_LOST and the program, if its exec succeeded,
 * runs unprotected. Any handle returned is released with owned_release
 * once owned_poll says keeper_reaped.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_owned_spawn(
    const maelys_sys_owned_options_t *options,
    maelys_sys_owned_t **out_owned);

/* The number of the program and of its group, for a diagnostic, never
 * for a kill; -1 for NULL or before STARTED. */
pid_t maelys_sys_owned_pid(const maelys_sys_owned_t *owned);

/* The control channel: readable when the keeper has something new, its
 * end included; to watch for writing when want_write says so. -1 for
 * NULL. Borrowed, closed by release. */
int maelys_sys_owned_fd(const maelys_sys_owned_t *owned);

/* Readable once the keeper has ended, whatever copies of the control
 * channel survive elsewhere. -1 for NULL. Borrowed, closed by release. */
int maelys_sys_owned_keeper_fd(const maelys_sys_owned_t *owned);

/*
 * Never blocks: pushes what waits to be written, reads every complete
 * record the keeper wrote, reaps the keeper when it has ended, and fills
 * *out_state. When the keeper's end is seen, what it wrote before is read
 * first, and a DONE there gives the final phase; without one, the
 * keeper's exit code does when it is a known terminal code, with
 * state_from_exit_code; otherwise KEEPER_LOST.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_owned_poll(
    maelys_sys_owned_t *owned,
    maelys_sys_owned_state_t *out_state);

/*
 * Asks the keeper to send signo to the leader, or to the group when
 * to_group is set. Never blocks: the request is written, or kept to be
 * written by poll and wait when the channel admits it, and *out_seq
 * names it for owned_result. ERR_WOULD_BLOCK when sixteen requests are
 * still unanswered: nothing is engaged, and the limit is what keeps
 * every answer from being lost or evicted before it is read. ERR_STATE
 * once the handle is final, or when the sequence numbers kept for
 * signals are spent.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_owned_signal(
    maelys_sys_owned_t *owned,
    int signo,
    int to_group,
    uint32_t *out_seq);

/*
 * Asks the keeper to stop the group: SIGTERM to the group, grace_ms or
 * the end of the leader, whichever first, then SIGKILL to the group, the
 * reaping of the leader and the observation of the group. Never blocks.
 * A second stop while one is under way names the first one again: the
 * first grace stands. ERR_STATE once the handle is final.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_owned_stop(
    maelys_sys_owned_t *owned,
    uint64_t grace_ms,
    uint32_t *out_seq);

/*
 * The answer to a request: OK with *out_errno the result of the kill (0,
 * or errno; ESRCH for a signal asked after the leader was reaped),
 * ERR_WOULD_BLOCK while the answer has not come, ERR_NOT_FOUND for a
 * sequence that was never issued or whose answer has been evicted (the
 * last sixteen are kept). The keeper runs a request only once it has
 * room for its answer, so an answer is never lost to a full channel:
 * it waits. Same thread as poll.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_owned_result(
    const maelys_sys_owned_t *owned,
    uint32_t seq,
    int *out_errno);

/*
 * Like poll, but sleeps on the two descriptors until the phase is final
 * and the keeper reaped, or until the absolute deadline: OK, or
 * ERR_TIMEOUT with *out_state filled all the same. INFINITE is
 * ERR_ARGUMENT.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_owned_wait(
    maelys_sys_owned_t *owned,
    uint64_t deadline_ms,
    maelys_sys_owned_state_t *out_state);

/*
 * Releases the handle and its descriptors once the keeper is reaped, and
 * sets *owned to NULL. Never blocks: it tries to reap the keeper once,
 * without waiting. ERR_STATE while the keeper lives: the handle is kept,
 * the group is still the caller's to follow, nothing is abandoned in
 * silence. ECHILD, the keeper reaped elsewhere against the contract,
 * releases all the same and is reported by poll as keeper_reaped_elsewhere
 * beforehand. NULL and an already-NULL handle are successes that do
 * nothing.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_owned_release(
    maelys_sys_owned_t **owned);

/* Where this build of the library expects the keeper to be installed:
 * libexec/maelys-sys/keeper under its prefix, or what the build variable
 * KEEPER_PATH said. Indicative; the caller names the path it trusts. */
const char *maelys_sys_owned_keeper_path(void);

#ifdef __cplusplus
}
#endif

#endif
