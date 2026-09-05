/*
 * File primitives. This unit carries the feature macros that make
 * O_NOFOLLOW, flock(2), renameat2(2) and renamex_np(2) visible on every
 * host, so the consumers do not have to.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "maelys/sys/file.h"

#include "maelys/sys/fd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif
#endif

/*
 * Fault point for the white-box test, which compiles this unit with
 * MAELYS_SYS_FILE_TESTING and provides file_fault: a non-zero return with
 * errno set fails the named step before the system call runs. The
 * function may also act on the file system and return zero, to stage a
 * change between two steps. Production builds compile it away.
 */
#ifdef MAELYS_SYS_FILE_TESTING
static int file_fault(const char *step);
static int file_fault_at(const char *step, const char *path);
#define FAULT(step) (file_fault(step) != 0)
#define FAULT_AT(step, path) (file_fault_at(step, path) != 0)
#else
#define FAULT(step) 0
#define FAULT_AT(step, path) 0
#endif

struct maelys_sys_file_lock {
    int fd;
};

/* ---- identity ---------------------------------------------------------- */

static void identity_from_stat(
    const struct stat *status, maelys_sys_file_identity_t *out) {
    out->device = status->st_dev;
    out->inode = status->st_ino;
    out->mode = status->st_mode;
    out->links = status->st_nlink;
    out->owner = status->st_uid;
    out->size = status->st_size < 0 ? 0u : (uint64_t)status->st_size;
}

static maelys_sys_file_mismatch_t check_expectations(
    const struct stat *status,
    const maelys_sys_file_expectations_t *expectations) {
    static const maelys_sys_file_expectations_t none = {0};
    if (!expectations) expectations = &none;
    if (!S_ISREG(status->st_mode)) return MAELYS_SYS_FILE_MISMATCH_TYPE;
    if (expectations->check_owner && status->st_uid != expectations->owner &&
        !(expectations->allow_root_owner && status->st_uid == 0)) {
        return MAELYS_SYS_FILE_MISMATCH_OWNER;
    }
    if (status->st_mode & expectations->forbidden_mode_bits) {
        return MAELYS_SYS_FILE_MISMATCH_MODE;
    }
    if (expectations->require_single_link && status->st_nlink != 1) {
        return MAELYS_SYS_FILE_MISMATCH_LINKS;
    }
    if (expectations->bound_size &&
        (status->st_size < 0 ||
         (uint64_t)status->st_size > expectations->maximum_size)) {
        return MAELYS_SYS_FILE_MISMATCH_SIZE;
    }
    return MAELYS_SYS_FILE_MATCH;
}

static maelys_sys_result_t verify_descriptor(
    int fd,
    const maelys_sys_file_expectations_t *expectations,
    maelys_sys_file_identity_t *out_identity,
    maelys_sys_file_mismatch_t *out_mismatch,
    struct stat *out_status) {
    struct stat status;
    if (FAULT("fstat") || fstat(fd, &status) != 0) return MAELYS_SYS_ERR_OS;
    maelys_sys_file_mismatch_t mismatch = check_expectations(&status, expectations);
    if (out_mismatch) *out_mismatch = mismatch;
    if (mismatch != MAELYS_SYS_FILE_MATCH) return MAELYS_SYS_ERR_IDENTITY;
    if (out_identity) identity_from_stat(&status, out_identity);
    if (out_status) *out_status = status;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_file_verify(
    int fd,
    const maelys_sys_file_expectations_t *expectations,
    maelys_sys_file_identity_t *out_identity,
    maelys_sys_file_mismatch_t *out_mismatch) {
    if (out_mismatch) *out_mismatch = MAELYS_SYS_FILE_MATCH;
    if (fd < 0) return MAELYS_SYS_ERR_ARGUMENT;
    return verify_descriptor(fd, expectations, out_identity, out_mismatch, NULL);
}

maelys_sys_result_t maelys_sys_file_path_identity(
    const char *path,
    maelys_sys_file_identity_t *out_identity) {
    struct stat status;
    if (!path || !out_identity) return MAELYS_SYS_ERR_ARGUMENT;
    if (FAULT("lstat") || lstat(path, &status) != 0) {
        return errno == ENOENT ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    }
    identity_from_stat(&status, out_identity);
    return MAELYS_SYS_OK;
}

int maelys_sys_file_identity_same(
    const maelys_sys_file_identity_t *first,
    const maelys_sys_file_identity_t *second) {
    return first && second && first->device == second->device &&
        first->inode == second->inode;
}

/* ---- opening ----------------------------------------------------------- */

/*
 * open(2) with the flags every path here takes, O_NONBLOCK held only for
 * the duration of open(2). The result classifies errno: a missing entry,
 * an object the kernel refuses to open as a plain file, or an OS error.
 */
static maelys_sys_result_t open_plain(
    const char *path, int flags, mode_t mode, int *out_fd) {
    *out_fd = -1;
    if (FAULT("open")) return MAELYS_SYS_ERR_OS;
    int fd = open(path, flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, mode);
    if (fd < 0) {
        switch (errno) {
            case ENOENT: return MAELYS_SYS_ERR_NOT_FOUND;
            case ELOOP:
            case EISDIR:
            case ENXIO:
            case EOPNOTSUPP:
                return MAELYS_SYS_ERR_IDENTITY;
            default: return MAELYS_SYS_ERR_OS;
        }
    }
    int current = FAULT("fcntl") ? -1 : fcntl(fd, F_GETFL);
    if (current < 0 || fcntl(fd, F_SETFL, current & ~O_NONBLOCK) != 0) {
        int saved = errno;
        (void)maelys_sys_fd_close(&fd);
        errno = saved;
        return MAELYS_SYS_ERR_OS;
    }
    *out_fd = fd;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_file_open_trusted(
    const char *path,
    const maelys_sys_file_expectations_t *expectations,
    maelys_sys_file_identity_t *out_identity,
    maelys_sys_file_mismatch_t *out_mismatch,
    int *out_fd) {
    if (out_mismatch) *out_mismatch = MAELYS_SYS_FILE_MATCH;
    if (out_fd) *out_fd = -1;
    if (!path || !out_fd) return MAELYS_SYS_ERR_ARGUMENT;
    int fd = -1;
    maelys_sys_result_t result = open_plain(path, O_RDONLY, 0, &fd);
    if (result == MAELYS_SYS_ERR_IDENTITY && out_mismatch) {
        *out_mismatch = MAELYS_SYS_FILE_MISMATCH_TYPE;
    }
    if (result != MAELYS_SYS_OK) return result;
    result = verify_descriptor(fd, expectations, out_identity, out_mismatch, NULL);
    if (result != MAELYS_SYS_OK) {
        int saved = errno;
        (void)maelys_sys_fd_close(&fd);
        errno = saved;
        return result;
    }
    *out_fd = fd;
    return MAELYS_SYS_OK;
}

/* ---- reading and syncing ----------------------------------------------- */

maelys_sys_result_t maelys_sys_file_read_bounded(
    int fd,
    void *buffer,
    size_t capacity,
    size_t *out_size) {
    if (out_size) *out_size = 0;
    if (fd < 0 || (!buffer && capacity) || !out_size) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    unsigned char *cursor = buffer;
    size_t filled = 0;
    while (filled < capacity) {
        ssize_t got = FAULT("read") ? -1 : read(fd, cursor + filled, capacity - filled);
        if (got < 0) {
            if (errno == EINTR) continue;
            return MAELYS_SYS_ERR_OS;
        }
        if (got == 0) {
            *out_size = filled;
            return MAELYS_SYS_OK;
        }
        filled += (size_t)got;
    }
    /* The buffer is full: only end of file proves the bound was met. */
    for (;;) {
        unsigned char probe;
        ssize_t got = FAULT("read") ? -1 : read(fd, &probe, 1u);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) return MAELYS_SYS_ERR_OS;
        if (got > 0) return MAELYS_SYS_ERR_CAPACITY;
        *out_size = filled;
        return MAELYS_SYS_OK;
    }
}

static maelys_sys_result_t sync_descriptor(int fd) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (!FAULT("fullfsync") && fcntl(fd, F_FULLFSYNC) == 0) return MAELYS_SYS_OK;
    /* Only a refusal falls back to fsync(2): an I/O error is a lost write,
     * not a volume without the feature. */
    if (errno != ENOTSUP && errno != EOPNOTSUPP && errno != ENODEV &&
        errno != EINVAL) {
        return MAELYS_SYS_ERR_OS;
    }
#endif
    if (FAULT("fsync") || fsync(fd) != 0) return MAELYS_SYS_ERR_OS;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_file_sync(int fd) {
    if (fd < 0) return MAELYS_SYS_ERR_ARGUMENT;
    return sync_descriptor(fd);
}

maelys_sys_result_t maelys_sys_directory_sync(const char *path) {
    if (!path) return MAELYS_SYS_ERR_ARGUMENT;
    if (FAULT_AT("open", path)) return MAELYS_SYS_ERR_OS;
    /* A directory is not a trusted object: a final symbolic link is
     * followed, as /tmp on macOS needs. */
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    maelys_sys_result_t result = sync_descriptor(fd);
    int saved = errno;
    if (maelys_sys_fd_close(&fd) != MAELYS_SYS_OK && result == MAELYS_SYS_OK) {
        return MAELYS_SYS_ERR_OS;
    }
    errno = saved;
    return result;
}

/* ---- exclusive write --------------------------------------------------- */

static int write_all(int fd, const unsigned char *bytes, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t amount = FAULT("write") ? -1 : write(fd, bytes + offset, length - offset);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) {
            if (amount == 0) errno = EIO;
            return -1;
        }
        offset += (size_t)amount;
    }
    return 0;
}

/* ---- conditional removal ------------------------------------------------ */

static maelys_sys_result_t remove_same(
    const char *path, const maelys_sys_file_identity_t *identity, int directory) {
    if (!path || !identity) return MAELYS_SYS_ERR_ARGUMENT;
    struct stat now;
    if (FAULT("lstat") || lstat(path, &now) != 0) {
        return errno == ENOENT ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    }
    if (now.st_dev != identity->device || now.st_ino != identity->inode ||
        (directory ? !S_ISDIR(now.st_mode) : S_ISDIR(now.st_mode))) {
        return MAELYS_SYS_ERR_IDENTITY;
    }
    /* Two calls: what a rename slips in between them is removed instead. */
    int removed = directory ? (FAULT("rmdir") ? -1 : rmdir(path))
                            : (FAULT("unlink") ? -1 : unlink(path));
    if (removed != 0) {
        return errno == ENOENT ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    }
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_file_unlink_same(
    const char *path,
    const maelys_sys_file_identity_t *identity) {
    return remove_same(path, identity, 0);
}

maelys_sys_result_t maelys_sys_directory_rmdir_same(
    const char *path,
    const maelys_sys_file_identity_t *identity) {
    return remove_same(path, identity, 1);
}

/* Removes path only if it still names the file that was created. */
static void remove_created(const char *path, const struct stat *created) {
    maelys_sys_file_identity_t identity;
    identity_from_stat(created, &identity);
    (void)maelys_sys_file_unlink_same(path, &identity);
}

maelys_sys_result_t maelys_sys_file_write_exclusive(
    const char *path,
    const void *bytes,
    size_t length,
    mode_t final_mode) {
    if (!path || (!bytes && length)) return MAELYS_SYS_ERR_ARGUMENT;
    if (FAULT("open")) return MAELYS_SYS_ERR_OS;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return errno == EEXIST ? MAELYS_SYS_ERR_EXISTS : MAELYS_SYS_ERR_OS;
    struct stat created;
    maelys_sys_result_t result = MAELYS_SYS_OK;
    if (FAULT("fstat") || fstat(fd, &created) != 0) {
        /* No identity to compare with: remove only what a fresh creation
         * looks like, an empty regular file of ours with a single link. */
        int saved = errno;
        struct stat now;
        (void)maelys_sys_fd_close(&fd);
        if (lstat(path, &now) == 0 && S_ISREG(now.st_mode) && now.st_size == 0 &&
            now.st_nlink == 1 && now.st_uid == geteuid()) {
            (void)unlink(path);
        }
        errno = saved;
        return MAELYS_SYS_ERR_OS;
    }
    if (write_all(fd, bytes, length) != 0 ||
        (FAULT("fchmod") || fchmod(fd, final_mode) != 0)) {
        result = MAELYS_SYS_ERR_OS;
    }
    if (result == MAELYS_SYS_OK) result = sync_descriptor(fd);
    int saved = errno;
    if (FAULT("close") || maelys_sys_fd_close(&fd) != MAELYS_SYS_OK) {
        if (result == MAELYS_SYS_OK) {
            result = MAELYS_SYS_ERR_OS;
            saved = errno;
        }
        (void)maelys_sys_fd_close(&fd);
    }
    if (result != MAELYS_SYS_OK) {
        remove_created(path, &created);
        errno = saved;
    }
    return result;
}

/* ---- publication ------------------------------------------------------- */

static int rename_noreplace(const char *staging, const char *destination) {
    if (FAULT("rename")) return -1;
#if defined(__APPLE__)
    return renamex_np(staging, destination, RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, AT_FDCWD, staging, AT_FDCWD, destination,
        (unsigned int)RENAME_NOREPLACE);
#else
    (void)staging;
    (void)destination;
    errno = ENOTSUP;
    return -1;
#endif
}

/*
 * The parent directory of a published name, followed through a final
 * symbolic link: a directory is not a trusted object here, and /tmp on
 * macOS is a link. Trailing separators do not count as a component.
 */
static maelys_sys_result_t sync_parent_of(const char *destination) {
    char parent[PATH_MAX];
    size_t end = strlen(destination);
    while (end > 1u && destination[end - 1u] == '/') --end;
    size_t slash = end;
    while (slash > 0u && destination[slash - 1u] != '/') --slash;
    size_t length;
    if (slash == 0u) {
        length = 1u;
        parent[0] = '.';
    } else if (slash == 1u) {
        length = 1u;
        parent[0] = '/';
    } else {
        length = slash - 1u;
        if (length >= sizeof(parent)) {
            errno = ENAMETOOLONG;
            return MAELYS_SYS_ERR_OS;
        }
        memcpy(parent, destination, length);
    }
    parent[length] = '\0';
    return maelys_sys_directory_sync(parent);
}

static maelys_sys_result_t publish(
    const char *staging,
    const char *destination,
    const maelys_sys_publish_options_t *options,
    int directory) {
    if (!staging || !destination) return MAELYS_SYS_ERR_ARGUMENT;
    struct stat source;
    if (FAULT("lstat") || lstat(staging, &source) != 0) {
        return errno == ENOENT ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    }
    if (directory ? !S_ISDIR(source.st_mode) : !S_ISREG(source.st_mode)) {
        return MAELYS_SYS_ERR_IDENTITY;
    }
    struct stat target;
    if (FAULT("lstat")) return MAELYS_SYS_ERR_OS;
    if (lstat(destination, &target) == 0) {
        /* The rename decides; this only settles the same-file case, which
         * the two hosts answer differently. */
        if (target.st_dev == source.st_dev && target.st_ino == source.st_ino) {
            return MAELYS_SYS_ERR_EXISTS;
        }
    } else if (errno != ENOENT) {
        return MAELYS_SYS_ERR_OS;
    }
    if (rename_noreplace(staging, destination) != 0) {
        switch (errno) {
            case EEXIST:
            case ENOTEMPTY:
                return MAELYS_SYS_ERR_EXISTS;
            case EINVAL:
            case ENOTSUP:
            case ENOSYS:
                return MAELYS_SYS_ERR_UNSUPPORTED;
            default:
                return MAELYS_SYS_ERR_OS;
        }
    }
    if (options && options->sync_parent) return sync_parent_of(destination);
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_file_publish_noreplace(
    const char *staging,
    const char *destination,
    const maelys_sys_publish_options_t *options) {
    return publish(staging, destination, options, 0);
}

maelys_sys_result_t maelys_sys_directory_publish_noreplace(
    const char *staging,
    const char *destination,
    const maelys_sys_publish_options_t *options) {
    return publish(staging, destination, options, 1);
}

/* ---- lock -------------------------------------------------------------- */

static void lock_default_expectations(maelys_sys_file_expectations_t *out) {
    memset(out, 0, sizeof(*out));
    out->check_owner = 1;
    out->owner = geteuid();
    out->forbidden_mode_bits = S_IRWXG | S_IRWXO;
    out->require_single_link = 1;
}

maelys_sys_result_t maelys_sys_file_lock_acquire(
    const char *path,
    const maelys_sys_file_lock_options_t *options,
    maelys_sys_file_lock_t **out_lock) {
    static const maelys_sys_file_lock_options_t defaults = {
        .exclusive = 1, .wait = 1, .create = 1, .writable = 0, .expectations = NULL
    };
    if (out_lock) *out_lock = NULL;
    if (!path || !out_lock) return MAELYS_SYS_ERR_ARGUMENT;
    if (!options) options = &defaults;
    maelys_sys_file_expectations_t fallback;
    const maelys_sys_file_expectations_t *expectations = options->expectations;
    if (!expectations) {
        lock_default_expectations(&fallback);
        expectations = &fallback;
    }
    int flags = (options->writable ? O_RDWR : O_RDONLY) |
        (options->create ? O_CREAT : 0);
    int fd = -1;
    maelys_sys_result_t result = open_plain(path, flags, 0600, &fd);
    if (result != MAELYS_SYS_OK) return result;
    struct stat before;
    result = verify_descriptor(fd, expectations, NULL, NULL, &before);
    if (result != MAELYS_SYS_OK) goto fail;
    int operation = (options->exclusive ? LOCK_EX : LOCK_SH) |
        (options->wait ? 0 : LOCK_NB);
    for (;;) {
        if (FAULT("flock")) { result = MAELYS_SYS_ERR_OS; goto fail; }
        if (flock(fd, operation) == 0) break;
        if (errno == EINTR) continue;
        result = errno == EWOULDBLOCK ? MAELYS_SYS_ERR_BUSY : MAELYS_SYS_ERR_OS;
        goto fail;
    }
    struct stat after;
    struct stat named;
    result = verify_descriptor(fd, expectations, NULL, NULL, &after);
    if (result != MAELYS_SYS_OK) goto unlock;
    if (FAULT("lstat") || lstat(path, &named) != 0) {
        result = errno == ENOENT ? MAELYS_SYS_ERR_IDENTITY : MAELYS_SYS_ERR_OS;
        goto unlock;
    }
    if (after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        named.st_dev != after.st_dev || named.st_ino != after.st_ino) {
        result = MAELYS_SYS_ERR_IDENTITY;
        goto unlock;
    }
    maelys_sys_file_lock_t *lock = calloc(1u, sizeof(*lock));
    if (!lock) { result = MAELYS_SYS_ERR_MEMORY; goto unlock; }
    lock->fd = fd;
    *out_lock = lock;
    return MAELYS_SYS_OK;
unlock:
    {
        int saved = errno;
        (void)flock(fd, LOCK_UN);
        errno = saved;
    }
fail:
    {
        int saved = errno;
        (void)maelys_sys_fd_close(&fd);
        errno = saved;
    }
    return result;
}

int maelys_sys_file_lock_fd(const maelys_sys_file_lock_t *lock) {
    return lock ? lock->fd : -1;
}

maelys_sys_result_t maelys_sys_file_lock_release(maelys_sys_file_lock_t **lock) {
    if (!lock || !*lock) return MAELYS_SYS_OK;
    maelys_sys_file_lock_t *owned = *lock;
    *lock = NULL;
    maelys_sys_result_t result = MAELYS_SYS_OK;
    if (flock(owned->fd, LOCK_UN) != 0) result = MAELYS_SYS_ERR_OS;
    int saved = errno;
    if (maelys_sys_fd_close(&owned->fd) != MAELYS_SYS_OK && result == MAELYS_SYS_OK) {
        result = MAELYS_SYS_ERR_OS;
        saved = errno;
    }
    free(owned);
    errno = saved;
    return result;
}
