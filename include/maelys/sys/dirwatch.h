#ifndef MAELYS_SYS_DIRWATCH_H
#define MAELYS_SYS_DIRWATCH_H

/*
 * Directory watching, in one sentence: "the entries of this directory
 * changed: reread it." No file name, no recursion, no distinction between a
 * creation, a removal and a rename, no file content. The caller rereads the
 * directory and compares with what it knew.
 *
 * Promised, alike on Linux (inotify) and macOS (kqueue):
 * - No silent loss. While an entry is live, every creation, removal or rename
 *   of a direct member of its directory yields at least one CHANGED, or an
 *   OVERFLOW that calls for a full reread. Replacing a member by rename(2)
 *   onto an existing name is a rename and is reported.
 * - False positives are allowed: a CHANGED says "reread", nothing more.
 * - Coalescing: between two polls an entry yields at most one change, however
 *   many events the kernel saw.
 * - Add before you read. What happens between the add and the caller's first
 *   read is reported like anything later; reading first loses what happens
 *   between the read and the add.
 *
 * Not promised, and chosen so that both hosts behave alike:
 * - A write in place to a member, or a change of its metadata, is not
 *   reported on either host. Linux could report them; they are not asked
 *   for, so that a caller written on Linux does not lean on a signal macOS
 *   never gives. A caller that wants to be told publishes by rename.
 * - The content of subdirectories is not watched: add each directory.
 * - No bound on latency.
 *
 * The entry follows the directory, not its path. Renaming the directory
 * itself, removing it, revoking or unmounting it yields GONE. Renaming one of
 * its ancestors yields nothing on either host, and the entry stays live: so
 * the absence of GONE does not say that the path still names this directory.
 *
 * The descriptor says what the kernel holds, not what this object still owes
 * the caller. Give poll an array of entry_capacity elements and call it once
 * each time the descriptor is readable: an entry is reported at most once
 * per call, so that array always fits, nothing stays pending here, and what
 * the kernel took in meanwhile keeps the descriptor readable. That is one
 * bounded call per turn of the loop, however fast anyone writes.
 *
 * With a smaller array, a call that fills it may leave changes pending here
 * with nothing to wake the loop. Poll again then, but a counted number of
 * times: while enough directories keep changing every call fills the array,
 * so "until a call does not fill it" may never come, and "until
 * ERR_WOULD_BLOCK" even less. A caller that stops on a full array arranges
 * its own return, a timer due at once, and does not wait on the descriptor.
 *
 * The descriptor may also be readable with nothing to report, as on Linux
 * after an entry is released, where the kernel acknowledges it: the poll
 * then says ERR_WOULD_BLOCK.
 */

#include <stddef.h>
#include <stdint.h>

#include "maelys/sys/loop.h"
#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct maelys_sys_dirwatch maelys_sys_dirwatch_t;
typedef uint64_t maelys_sys_dirwatch_entry_t;

typedef enum maelys_sys_dirwatch_flags {
    /* The entries of the directory changed: reread it. */
    MAELYS_SYS_DIRWATCH_CHANGED = 1u << 0,
    /* The directory was removed, renamed, revoked or unmounted. The entry is
     * released by the poll that delivers this. */
    MAELYS_SYS_DIRWATCH_GONE = 1u << 1,
    /* The kernel dropped events: every live entry reports CHANGED|OVERFLOW
     * once; reread them all. Only inotify overflows. */
    MAELYS_SYS_DIRWATCH_OVERFLOW = 1u << 2
} maelys_sys_dirwatch_flags_t;

/* Fields fixed within ABI 1. */
typedef struct maelys_sys_dirwatch_change {
    maelys_sys_token_t token;
    maelys_sys_dirwatch_entry_t entry;
    unsigned flags;
} maelys_sys_dirwatch_change_t;

/*
 * entry_capacity bounds the live entries; 0 is ERR_ARGUMENT. One kernel
 * object per handle: an inotify instance on Linux, a kqueue of its own on
 * macOS; ERR_UNSUPPORTED on any other host, where the caller keeps its
 * periodic rereads. On macOS every entry holds one open directory
 * descriptor: size entry_capacity against RLIMIT_NOFILE, 256 by default
 * there; nothing here raises that limit. Like a loop, the handle belongs to
 * the thread that created it: add, remove, poll and destroy from another
 * thread are ERR_STATE.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_dirwatch_create(
    size_t entry_capacity,
    maelys_sys_dirwatch_t **out_dirwatch);

/*
 * Borrowed readable descriptor, valid until destroy and never closed by the
 * caller; -1 for NULL. Watch it in a loop for READ, then poll once with an
 * array of entry_capacity elements. Both the inotify descriptor and the
 * kqueue are watchable by every loop backend of their host.
 */
int maelys_sys_dirwatch_fd(const maelys_sys_dirwatch_t *dirwatch);

/*
 * Starts watching the directory path names, and gives it token. Add, then
 * read the directory. A symbolic link as final component is ERR_IDENTITY, a
 * non-directory ERR_ARGUMENT, a missing path ERR_NOT_FOUND. A directory
 * already watched by this handle, by this or any other path, is ERR_EXISTS:
 * one entry per directory, and a caller that wants several tokens
 * multiplexes its own. ERR_CAPACITY beyond entry_capacity. A kernel limit
 * (inotify's max_user_watches, ENOSPC; descriptors, EMFILE or ENFILE) is
 * ERR_OS with errno, as is a directory the caller may not read on Linux,
 * where macOS asks for no such permission. The path is borrowed for the call
 * only. On failure *out_entry is 0, which is never a live entry.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_dirwatch_add(
    maelys_sys_dirwatch_t *dirwatch,
    const char *path,
    maelys_sys_token_t token,
    maelys_sys_dirwatch_entry_t *out_entry);

/*
 * Releases the entry and any change still pending for it. ERR_NOT_FOUND for
 * an entry already released, including one released by a delivered GONE.
 * Entry numbers are never reused within a handle.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_dirwatch_remove(
    maelys_sys_dirwatch_t *dirwatch,
    maelys_sys_dirwatch_entry_t entry);

/*
 * Never blocks, and does not wait for the directories to fall quiet: it
 * takes what the kernel held when the call began, coalesces per entry and
 * fills at most capacity changes, each entry at most once. What the kernel
 * takes in during the call is left for the next one and keeps the descriptor
 * readable. So one call costs what was queued, bounded by the kernel's own
 * queue on Linux and by entry_capacity on macOS, however fast anyone writes.
 * ERR_WOULD_BLOCK when nothing is pending, the normal state; *out_count is
 * then 0. Changes that do not fit stay pending for a later call, and entries
 * are served in turn, so one that keeps changing cannot hold the others
 * back. An entry whose GONE this call delivers is released before the call
 * returns; a GONE that did not fit keeps its entry live until delivered.
 * changes must hold capacity elements, capacity at least 1.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_dirwatch_poll(
    maelys_sys_dirwatch_t *dirwatch,
    maelys_sys_dirwatch_change_t *changes,
    size_t capacity,
    size_t *out_count);

/*
 * Releases every entry and the kernel object, and sets *dirwatch to NULL.
 * Unwatch the descriptor from any loop first, as loop.h requires. NULL and
 * an already-NULL handle are successes that do nothing.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_dirwatch_destroy(
    maelys_sys_dirwatch_t **dirwatch);

#ifdef __cplusplus
}
#endif

#endif
