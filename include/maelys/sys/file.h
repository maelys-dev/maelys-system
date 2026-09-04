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
 * Contracted file systems: local ones with the no-replace rename flag
 * (ext4, xfs, btrfs, tmpfs and overlayfs on Linux 3.15 and later; APFS and
 * HFS+). NFS, SMB, exFAT and FUSE file systems are outside every guarantee
 * below, for publication atomicity, locking and durability alike; the
 * functions report what the kernel refuses and detect nothing else.
 *
 * errno is meaningful on ERR_OS only, as everywhere in this library.
 */

/*
 * What the caller expects of a regular file before trusting it. A
 * zero-initialised value (`= {0}`), or NULL wherever a pointer to it is
 * accepted, requires only a regular file; each field adds a check. Every
 * product surveyed wanted some subset of these; none wanted anything else.
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
    /* Reject a file larger than maximum_size bytes when bound_size is set;
     * with maximum_size 0 only an empty file passes. */
    int bound_size;
    uint64_t maximum_size;
} maelys_sys_file_expectations_t;

/* Which expectation a file failed. TYPE also covers what open(2) refused
 * before any check: a symbolic link (ELOOP), a directory, a FIFO or a
 * socket the kernel would not open as a file. */
typedef enum maelys_sys_file_mismatch {
    MAELYS_SYS_FILE_MATCH = 0,
    MAELYS_SYS_FILE_MISMATCH_TYPE,
    MAELYS_SYS_FILE_MISMATCH_OWNER,
    MAELYS_SYS_FILE_MISMATCH_MODE,
    MAELYS_SYS_FILE_MISMATCH_LINKS,
    MAELYS_SYS_FILE_MISMATCH_SIZE
} maelys_sys_file_mismatch_t;

/*
 * A snapshot of a file's identity, taken by verify or by path_identity.
 * POSIX scalars only; the library exposes no struct stat. The fields are
 * fixed: a consumer's copy compiled against this ABI stays complete.
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
 * file or any expectation fails; *out_mismatch then says which, when the
 * pointer is given. On ERR_OS errno identifies fstat(2). out_identity and
 * out_mismatch may be NULL.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_verify(
    int fd,
    const maelys_sys_file_expectations_t *expectations,
    maelys_sys_file_identity_t *out_identity,
    maelys_sys_file_mismatch_t *out_mismatch);

/*
 * Identity of the object a path names, without following a final symbolic
 * link. A symbolic link is reported as such through mode; the function
 * itself refuses nothing. ERR_NOT_FOUND when nothing is there.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_path_identity(
    const char *path,
    maelys_sys_file_identity_t *out_identity);

/*
 * Same file: same device and inode, nothing else. Size, mode, links and
 * owner are data about the file at snapshot time, not its identity; a lock
 * file that grew is still the locked file. NULL is never the same.
 */
int maelys_sys_file_identity_same(
    const maelys_sys_file_identity_t *first,
    const maelys_sys_file_identity_t *second);

/*
 * Opens a regular file the caller intends to trust: read-only, close-on-exec,
 * not following a final symbolic link, and with O_NONBLOCK during open(2)
 * so that a FIFO planted at the path cannot suspend the caller; the flag is
 * cleared before the descriptor is returned. The descriptor is then checked
 * with maelys_sys_file_verify. What open(2) itself refuses because the
 * object is not a plain file (ELOOP, EISDIR, ENXIO, EOPNOTSUPP) is
 * ERR_IDENTITY with MISMATCH_TYPE, so the same object gives the same result
 * on both hosts. On ERR_IDENTITY no descriptor is returned and no data was
 * read. The caller owns *out_fd. ERR_NOT_FOUND when nothing is there.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_open_trusted(
    const char *path,
    const maelys_sys_file_expectations_t *expectations,
    maelys_sys_file_identity_t *out_identity,
    maelys_sys_file_mismatch_t *out_mismatch,
    int *out_fd);

/*
 * Reads the whole file from its current offset into the caller's buffer.
 * The bound is the buffer: the function reads until end of file and fails
 * with ERR_CAPACITY when the file holds more than capacity bytes, which
 * also catches a file that grows while it is read; a file of exactly
 * capacity bytes succeeds. A file that shrinks simply ends early and
 * *out_size says how much arrived. EINTR is retried. After ERR_CAPACITY
 * the descriptor's offset is unspecified. Nothing is allocated: size the
 * buffer from the identity that verify returned, or from a fixed limit.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_read_bounded(
    int fd,
    void *buffer,
    size_t capacity,
    size_t *out_size);

/*
 * Makes a descriptor's content durable. Linux: fsync(2). macOS: fsync(2)
 * leaves data in the drive's cache, so fcntl(F_FULLFSYNC) is issued first
 * and fsync(2) only when the volume refuses it; the guarantee there is the
 * best the platform offers, as the close-on-exec guarantee already is.
 * Content only: the directory entry needs directory_sync or a publish with
 * sync_parent.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_sync(int fd);

/* Same for a directory's entries, opened by path without following a link.
 * On Linux this is the documented way to persist a rename, link or unlink. */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_directory_sync(
    const char *path);

/*
 * Creates path exclusively (ERR_EXISTS if anything is there, a symbolic
 * link included), writes every byte, applies final_mode, makes the content
 * durable as file_sync does, and closes. The file is created with mode
 * 0600 under the umask and only receives final_mode, which fchmod(2)
 * applies as given without the umask, after its content, so a partial file
 * is never readable under the final permissions. On any failure the file
 * is removed, by unlink(2) of path only if path still names the file that
 * was created, and the failure reported. bytes may be NULL when length is
 * 0. Durability covers the file, not its directory entry.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_write_exclusive(
    const char *path,
    const void *bytes,
    size_t length,
    mode_t final_mode);

typedef struct maelys_sys_publish_options {
    /* Also make the destination's parent directory durable on success. The
     * parent is the destination path up to its last separator (trailing
     * separators do not count), resolved again after the rename and
     * followed through a final symbolic link: a directory is not a trusted
     * object here, and /tmp on macOS is a link. */
    int sync_parent;
} maelys_sys_publish_options_t;

/*
 * Publishes a staged regular file under a new name: the destination
 * appears atomically and is never replaced, by renameat2(RENAME_NOREPLACE)
 * on Linux and renamex_np(RENAME_EXCL) on macOS. staging must name a
 * regular file without following a link (ERR_IDENTITY otherwise: the two
 * hosts disagree on what linking a symbolic link means, so it is refused
 * rather than interpreted). ERR_EXISTS when the destination already
 * exists, whatever it is, and when staging and destination already name
 * the same file; the staged file is then left in place. When the file
 * system refuses the flag (EINVAL on Linux, ENOTSUP on macOS) the call
 * fails with ERR_UNSUPPORTED and nothing moved: there is no fallback, the
 * unsafe one of checking the destination and then renaming least of all,
 * and link(2) is not exclusive on every contracted file system nor
 * atomic for a reader counting links. options may be NULL.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_publish_noreplace(
    const char *staging,
    const char *destination,
    const maelys_sys_publish_options_t *options);

/*
 * Same for a directory. EINVAL also names a destination inside the staged
 * directory itself; it is reported as ERR_UNSUPPORTED like a refused flag,
 * nothing moved in either case.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_directory_publish_noreplace(
    const char *staging,
    const char *destination,
    const maelys_sys_publish_options_t *options);

/*
 * Advisory lock on a file whose identity is checked before and after the
 * lock is taken. The handle owns the descriptor. The lock belongs to the
 * open file description and lives as long as any descriptor of it does:
 * until release, until the process ends, or in a child that inherited the
 * descriptor across fork(2) until that child closes it, which is why a
 * process launcher closes inherited descriptors after fork as fd.h
 * already requires. It never ends because another descriptor on the same
 * file was closed elsewhere in the process, which is why flock(2) is used
 * and not fcntl locks. Never combine it with fcntl(2) locks on the same
 * file: macOS makes the two kinds conflict, Linux does not. Two processes
 * on two hosts sharing a network file system may both succeed; that is a
 * reason not to put a lock there.
 */
typedef struct maelys_sys_file_lock maelys_sys_file_lock_t;

typedef struct maelys_sys_file_lock_options {
    /* Exclusive (LOCK_EX) rather than shared (LOCK_SH). */
    int exclusive;
    /* Wait for the lock; otherwise ERR_BUSY when it is held elsewhere.
     * flock(2) offers no deadline: a caller that needs one polls with
     * wait off and its own absolute deadline. */
    int wait;
    /* Create the lock file when absent, mode 0600 under the umask. Off:
     * ERR_NOT_FOUND. A symbolic link at the path is never followed nor
     * replaced: ERR_IDENTITY. */
    int create;
    /* Open the file read-write, for a holder that records something in
     * it. Off: read-only, which flock(2) accepts on both hosts and which a
     * mode 0400 file allows. */
    int writable;
    /* Applied to the file before the lock and re-checked after it. NULL
     * requires a regular file with exactly one link, owned by the caller,
     * with no group or other permission bits. */
    const maelys_sys_file_expectations_t *expectations;
} maelys_sys_file_lock_options_t;

/*
 * Opens path close-on-exec, without following a final symbolic link, with
 * O_NONBLOCK during open(2) then cleared; verifies the descriptor; takes
 * the lock, retrying on EINTR when waiting; then applies the expectations
 * again to the locked descriptor and confirms that path still names the
 * locked file (same device and inode). The second pass re-applies the
 * expectations, not the identity: a size bound is checked twice.
 * Any mismatch, and what open(2) refuses as not a plain file, releases
 * everything and reports ERR_IDENTITY: a file replaced between open and
 * lock is not a file the caller holds. options may be NULL: exclusive,
 * waiting, creating, read-only, default expectations.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_lock_acquire(
    const char *path,
    const maelys_sys_file_lock_options_t *options,
    maelys_sys_file_lock_t **out_lock);

/* Borrowed descriptor of the locked file, valid until release; -1 for NULL. */
int maelys_sys_file_lock_fd(const maelys_sys_file_lock_t *lock);

/*
 * Unlocks, closes and frees; sets *lock to NULL first. NULL and an
 * already-NULL handle are idempotent successes. release never removes the
 * lock file. A holder of the exclusive lock may unlink it itself: another
 * user of this function that then locks the orphaned inode sees the path
 * diverge and gets ERR_IDENTITY, but a program locking the same file by
 * other means does not, so the choice is the caller's.
 */
MAELYS_SYS_NODISCARD maelys_sys_result_t maelys_sys_file_lock_release(
    maelys_sys_file_lock_t **lock);

#ifdef __cplusplus
}
#endif

#endif
