#ifndef MAELYS_SYS_FILE_H
#define MAELYS_SYS_FILE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "maelys/sys/result.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mechanical file operations that several Maelys products had each written
 * for themselves: verifying that a file is the one the caller trusts,
 * reading it within a bound, writing a new file durably, publishing a
 * staged file or directory without replacing anything, and holding an
 * advisory lock on a file whose identity has been checked.
 *
 * Every function here opens paths with close-on-exec and without following
 * a symbolic link at the last component, in a translation unit compiled
 * with the feature macros that make those flags visible on every host, so
 * no consumer carries an #ifdef O_NOFOLLOW again. Paths are borrowed for
 * the duration of the call. No function walks a path for the caller,
 * chooses a directory, applies a naming rule, hashes content or decides a
 * permission policy: those remain product decisions.
 *
 * Contracted file systems: local ones. NFS and SMB are outside every
 * guarantee below (atomicity of no-replace publication, locking, and
 * durability); the header says so once and the functions do not detect it.
 */

/*
 * What the caller expects of a regular file before trusting it. A
 * zero-initialised value (`= {0}`) requires only a regular file; each field
 * adds a check. Every product surveyed wanted some subset of these; none
 * wanted anything else.
 */
typedef struct maelys_sys_file_expectations {
    /* Required owner, typically geteuid(). Checked only when check_owner. */
    int check_owner;
    uid_t owner;
    /* Also accept a file owned by uid 0 when check_owner is set. */
    int allow_root_owner;
    /* Permission bits that must all be clear, e.g. S_IRWXG | S_IRWXO for a
     * private file, S_IWGRP | S_IWOTH for a file others may read. */
    mode_t forbidden_mode_bits;
    /* Require exactly one hard link: no alias can rewrite it elsewhere. */
    int require_single_link;
    /* Reject a file larger than maximum_size bytes when bound_size is set. */
    int bound_size;
    uint64_t maximum_size;
} maelys_sys_file_expectations_t;

/*
 * A snapshot of a file's identity, taken by verify or by path_identity and
 * compared later with identity_same to prove that the file seen first is
 * the file seen last. POSIX scalars only; the library exposes no struct
 * stat.
 */
typedef struct maelys_sys_file_identity {
    dev_t device;
    ino_t inode;
    mode_t mode;
    nlink_t links;
    uid_t owner;
    uint64_t size;
} maelys_sys_file_identity_t;

/*
 * Checks an open descriptor against the expectations with fstat(2) and
 * records its identity. ERR_IDENTITY when the descriptor is not a regular
 * file or any expectation fails; errno is then EPERM for ownership and
 * mode, EMLINK for the link count, EFBIG for the size, EISDIR or EINVAL for
 * the file type. On ERR_OS errno identifies fstat(2). out_identity may be
 * NULL.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_verify(
    int fd,
    const maelys_sys_file_expectations_t *expectations,
    maelys_sys_file_identity_t *out_identity);

/*
 * Identity of the object a path names, without following a final symbolic
 * link. A symbolic link is reported as such through mode and rejected by
 * verify-style callers; the function itself refuses nothing. ERR_NOT_FOUND
 * when nothing is there.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_path_identity(
    const char *path,
    maelys_sys_file_identity_t *out_identity);

/* Same device, inode, mode, link count, owner and size. NULL is never equal. */
int maelys_sys_file_identity_same(
    const maelys_sys_file_identity_t *first,
    const maelys_sys_file_identity_t *second);

/*
 * Opens a regular file the caller intends to trust: read-only, close-on-exec,
 * not following a final symbolic link, and with O_NONBLOCK during open(2)
 * so that a FIFO planted at the path cannot suspend the caller; the flag is
 * cleared before the descriptor is returned. The descriptor is then checked
 * with maelys_sys_file_verify. On ERR_IDENTITY nothing is returned and
 * the file is left untouched. The caller owns *out_fd.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_open_trusted(
    const char *path,
    const maelys_sys_file_expectations_t *expectations,
    maelys_sys_file_identity_t *out_identity,
    int *out_fd);

/*
 * Reads the whole file from its current offset into the caller's buffer.
 * The bound is the buffer: the function reads until end of file and fails
 * with ERR_CAPACITY when the file holds more than capacity bytes, which
 * also catches a file that grows while it is read; a file that shrinks
 * simply ends early and *out_size says how much arrived. EINTR is retried.
 * Nothing is allocated: size the buffer from the identity that verify
 * returned, or from a fixed limit.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_read_bounded(
    int fd,
    void *buffer,
    size_t capacity,
    size_t *out_size);

/*
 * Creates path exclusively (ERR_EXISTS if anything is there, a symbolic
 * link included), writes every byte, applies final_mode, fsync(2)s and
 * closes; a failure at any point removes the file and reports it. The file
 * is created with mode 0600 and only receives final_mode after its content,
 * so a partial file is never readable under the final permissions. bytes
 * may be NULL when length is 0. Durability covers the file, not the
 * directory entry: publish the file or call directory_sync for that.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_write_exclusive(
    const char *path,
    const void *bytes,
    size_t length,
    mode_t final_mode);

/*
 * Makes a directory's entries durable: fsync(2) on the directory itself.
 * On Linux this is the documented way to persist a rename, link or
 * unlink. On macOS fsync(2) does not reach the medium; the function issues
 * fcntl(F_FULLFSYNC) and falls back to fsync(2) when the volume refuses it,
 * so the guarantee there is the best the platform offers, as the
 * close-on-exec guarantee already is.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_directory_sync(
    const char *path);

typedef struct maelys_sys_publish_options {
    /* Also make the destination's parent directory durable on success. */
    int sync_parent;
} maelys_sys_publish_options_t;

/*
 * Publishes a staged file under a new name: the destination appears
 * atomically and is never replaced. ERR_EXISTS when the destination
 * already exists, whatever it is, and the staged file is left in place.
 * The mechanism is renameat2(RENAME_NOREPLACE) on Linux and
 * renamex_np(RENAME_EXCL) on macOS; when the file system refuses the flag,
 * the file is published by link(2), which is exclusive by construction,
 * then the staged name is removed. The unsafe fallback of checking the
 * destination and then renaming is never used. If publication succeeded
 * but the staged name could not be removed, the call is a success and
 * errno holds the unlink(2) error: the destination is what matters, the
 * caller may retry the removal. options may be NULL.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_publish_noreplace(
    const char *staging,
    const char *destination,
    const maelys_sys_publish_options_t *options);

/*
 * Same for a directory. There is no exclusive fallback for a directory:
 * when the file system refuses RENAME_NOREPLACE the call fails with
 * ERR_UNSUPPORTED and nothing moved. Contracted on ext4, xfs, btrfs and
 * tmpfs (Linux 3.15 and later) and on APFS and HFS+.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_directory_publish_noreplace(
    const char *staging,
    const char *destination,
    const maelys_sys_publish_options_t *options);

/*
 * Advisory lock on a file whose identity is checked before and after the
 * lock is taken. The handle owns the descriptor; the lock belongs to that
 * open file description and ends with release, with the process, or with
 * a crash, never because another descriptor on the same file was closed
 * elsewhere in the process, which is why flock(2) is used and not fcntl
 * locks. Two processes on two hosts sharing a network file system may both
 * succeed; that is a reason not to put a lock there.
 */
typedef struct maelys_sys_file_lock maelys_sys_file_lock_t;

typedef struct maelys_sys_file_lock_options {
    /* Exclusive (LOCK_EX) rather than shared (LOCK_SH). */
    int exclusive;
    /* Wait for the lock; otherwise ERR_BUSY when it is held elsewhere. */
    int wait;
    /* Create the lock file when absent, mode 0600. Off: ERR_NOT_FOUND. */
    int create;
    /* Applied to the file before the lock and re-checked after it. NULL
     * requires a regular file with exactly one link, owned by the caller,
     * with no group or other permission bits. */
    const maelys_sys_file_expectations_t *expectations;
} maelys_sys_file_lock_options_t;

/*
 * Opens path read-write, close-on-exec, without following a final symbolic
 * link, with O_NONBLOCK during open(2) then cleared; verifies the
 * descriptor; takes the lock, retrying on EINTR when waiting; then verifies
 * again and confirms that path still names the locked file (same device
 * and inode). Any mismatch releases everything and reports ERR_IDENTITY:
 * a file replaced between open and lock is not a file the caller holds.
 * options may be NULL: exclusive, waiting, creating, default expectations.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_lock_acquire(
    const char *path,
    const maelys_sys_file_lock_options_t *options,
    maelys_sys_file_lock_t **out_lock);

/* Borrowed descriptor of the locked file, valid until release; -1 for NULL. */
int maelys_sys_file_lock_fd(const maelys_sys_file_lock_t *lock);

/*
 * Unlocks, closes and frees; sets *lock to NULL first. NULL and an
 * already-NULL handle are idempotent successes. The lock file is never
 * removed: removing it while another process holds a descriptor would let
 * two holders coexist.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_lock_release(
    maelys_sys_file_lock_t **lock);

#ifdef __cplusplus
}
#endif

#endif
