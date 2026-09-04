/* POSIX.1-2008 for lstat, symlink, mkfifo and mkdtemp under -std=c11. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* mkdtemp is hidden from strict POSIX on macOS */
#endif

#include "maelys/sys.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s (errno %d: %s)\n", \
            __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return 1; \
    } \
} while (0)

static char work[256];

static int path_in_work(const char *name, char *out, size_t capacity) {
    int written = snprintf(out, capacity, "%s/%s", work, name);
    return written > 0 && (size_t)written < capacity;
}

static int write_plain(const char *name, const char *content, mode_t mode) {
    char path[512];
    if (!path_in_work(name, path, sizeof(path))) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t length = strlen(content);
    if (write(fd, content, length) != (ssize_t)length) { close(fd); return -1; }
    if (fchmod(fd, mode) != 0) { close(fd); return -1; }
    return close(fd);
}

static int test_verify_and_open(void) {
    char path[512], other[512];
    maelys_sys_file_expectations_t expect = {0};
    maelys_sys_file_identity_t identity, again;
    maelys_sys_file_mismatch_t mismatch = MAELYS_SYS_FILE_MATCH;
    int fd = -1;

    CHECK(write_plain("plain", "hello", 0600) == 0);
    CHECK(path_in_work("plain", path, sizeof(path)));
    expect.check_owner = 1;
    expect.owner = geteuid();
    expect.forbidden_mode_bits = S_IRWXG | S_IRWXO;
    expect.require_single_link = 1;
    expect.bound_size = 1;
    expect.maximum_size = 5u;
    CHECK(maelys_sys_file_open_trusted(path, &expect, &identity, &mismatch, &fd) ==
        MAELYS_SYS_OK);
    CHECK(fd >= 0 && mismatch == MAELYS_SYS_FILE_MATCH);
    CHECK(identity.size == 5u && identity.links == 1u && identity.owner == geteuid());
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) == 0);
    CHECK(maelys_sys_file_path_identity(path, &again) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_identity_same(&identity, &again));
    CHECK(!maelys_sys_file_identity_same(&identity, NULL));
    /* NULL expectations only require a regular file. */
    CHECK(maelys_sys_file_verify(fd, NULL, NULL, &mismatch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&fd) == MAELYS_SYS_OK);

    /* Size bound: exactly the bound passes, one byte more fails. */
    expect.maximum_size = 4u;
    CHECK(maelys_sys_file_open_trusted(path, &expect, NULL, &mismatch, &fd) ==
        MAELYS_SYS_ERR_IDENTITY);
    CHECK(mismatch == MAELYS_SYS_FILE_MISMATCH_SIZE && fd == -1);
    expect.maximum_size = 5u;

    /* Mode bits. */
    CHECK(chmod(path, 0640) == 0);
    CHECK(maelys_sys_file_open_trusted(path, &expect, NULL, &mismatch, &fd) ==
        MAELYS_SYS_ERR_IDENTITY);
    CHECK(mismatch == MAELYS_SYS_FILE_MISMATCH_MODE);
    CHECK(chmod(path, 0600) == 0);

    /* A hard link alias. */
    CHECK(path_in_work("alias", other, sizeof(other)));
    CHECK(link(path, other) == 0);
    CHECK(maelys_sys_file_open_trusted(path, &expect, NULL, &mismatch, &fd) ==
        MAELYS_SYS_ERR_IDENTITY);
    CHECK(mismatch == MAELYS_SYS_FILE_MISMATCH_LINKS);
    CHECK(unlink(other) == 0);

    /* Owner: root not allowed by default, allowed on request; a file the
     * caller owns passes either way. */
    expect.owner = geteuid() + 1u;
    CHECK(maelys_sys_file_open_trusted(path, &expect, NULL, &mismatch, &fd) ==
        MAELYS_SYS_ERR_IDENTITY);
    CHECK(mismatch == MAELYS_SYS_FILE_MISMATCH_OWNER);
    expect.owner = geteuid();

    /* Not a plain file: symbolic link, directory, FIFO. */
    CHECK(path_in_work("link", other, sizeof(other)));
    CHECK(symlink(path, other) == 0);
    CHECK(maelys_sys_file_open_trusted(other, &expect, NULL, &mismatch, &fd) ==
        MAELYS_SYS_ERR_IDENTITY);
    CHECK(mismatch == MAELYS_SYS_FILE_MISMATCH_TYPE && fd == -1);
    CHECK(maelys_sys_file_path_identity(other, &again) == MAELYS_SYS_OK);
    CHECK(S_ISLNK(again.mode));
    CHECK(unlink(other) == 0);
    CHECK(maelys_sys_file_open_trusted(work, &expect, NULL, &mismatch, &fd) ==
        MAELYS_SYS_ERR_IDENTITY);
    CHECK(mismatch == MAELYS_SYS_FILE_MISMATCH_TYPE);
    CHECK(path_in_work("fifo", other, sizeof(other)));
    CHECK(mkfifo(other, 0600) == 0);
    /* No writer: a plain open(2) would block here. */
    CHECK(maelys_sys_file_open_trusted(other, &expect, NULL, &mismatch, &fd) ==
        MAELYS_SYS_ERR_IDENTITY);
    CHECK(mismatch == MAELYS_SYS_FILE_MISMATCH_TYPE);
    CHECK(unlink(other) == 0);

    CHECK(path_in_work("missing", other, sizeof(other)));
    CHECK(maelys_sys_file_open_trusted(other, &expect, NULL, &mismatch, &fd) ==
        MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_file_path_identity(other, &again) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_file_open_trusted(NULL, &expect, NULL, NULL, &fd) ==
        MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_file_verify(-1, NULL, NULL, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    return 0;
}

static int test_read_bounded(void) {
    char path[512];
    unsigned char buffer[8];
    size_t size = 99u;
    int fd = -1;
    CHECK(write_plain("bounded", "12345678", 0600) == 0);
    CHECK(path_in_work("bounded", path, sizeof(path)));
    CHECK(maelys_sys_file_open_trusted(path, NULL, NULL, NULL, &fd) == MAELYS_SYS_OK);
    /* Exactly the capacity succeeds. */
    CHECK(maelys_sys_file_read_bounded(fd, buffer, 8u, &size) == MAELYS_SYS_OK);
    CHECK(size == 8u && memcmp(buffer, "12345678", 8u) == 0);
    /* One byte less than the file: capacity exceeded. */
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(maelys_sys_file_read_bounded(fd, buffer, 7u, &size) == MAELYS_SYS_ERR_CAPACITY);
    /* Room to spare: the actual size comes back. */
    CHECK(lseek(fd, 3, SEEK_SET) == 3);
    CHECK(maelys_sys_file_read_bounded(fd, buffer, 8u, &size) == MAELYS_SYS_OK);
    CHECK(size == 5u && memcmp(buffer, "45678", 5u) == 0);
    /* An empty buffer accepts only an exhausted file. */
    CHECK(maelys_sys_file_read_bounded(fd, NULL, 0u, &size) == MAELYS_SYS_OK);
    CHECK(size == 0u);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(maelys_sys_file_read_bounded(fd, NULL, 0u, &size) == MAELYS_SYS_ERR_CAPACITY);
    CHECK(maelys_sys_file_read_bounded(fd, NULL, 4u, &size) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_fd_close(&fd) == MAELYS_SYS_OK);
    return 0;
}

static int test_write_exclusive_and_sync(void) {
    char path[512], other[512];
    struct stat status;
    mode_t previous = umask(077);
    int fd = -1;
    CHECK(path_in_work("written", path, sizeof(path)));
    CHECK(maelys_sys_file_write_exclusive(path, "abc", 3u, 0644) == MAELYS_SYS_OK);
    (void)umask(previous);
    CHECK(lstat(path, &status) == 0 && S_ISREG(status.st_mode));
    /* fchmod applies the final mode as given, whatever the umask. */
    CHECK((status.st_mode & 0777) == 0644 && status.st_size == 3);
    CHECK(maelys_sys_file_write_exclusive(path, "x", 1u, 0600) == MAELYS_SYS_ERR_EXISTS);
    CHECK(lstat(path, &status) == 0 && status.st_size == 3);
    /* A symbolic link at the path is "something there". */
    CHECK(path_in_work("written-link", other, sizeof(other)));
    CHECK(symlink("nowhere", other) == 0);
    CHECK(maelys_sys_file_write_exclusive(other, "x", 1u, 0600) == MAELYS_SYS_ERR_EXISTS);
    CHECK(unlink(other) == 0);
    /* An empty file. */
    CHECK(path_in_work("empty", other, sizeof(other)));
    CHECK(maelys_sys_file_write_exclusive(other, NULL, 0u, 0600) == MAELYS_SYS_OK);
    CHECK(lstat(other, &status) == 0 && status.st_size == 0);
    /* A missing parent is an OS error and leaves nothing behind. */
    CHECK(path_in_work("no-such-dir/file", other, sizeof(other)));
    errno = 0;
    CHECK(maelys_sys_file_write_exclusive(other, "x", 1u, 0600) == MAELYS_SYS_ERR_OS);
    CHECK(errno == ENOENT);
    CHECK(maelys_sys_file_write_exclusive(NULL, "x", 1u, 0600) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_file_write_exclusive(other, NULL, 1u, 0600) == MAELYS_SYS_ERR_ARGUMENT);

    /* Syncing. */
    CHECK(maelys_sys_file_open_trusted(path, NULL, NULL, NULL, &fd) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_sync(fd) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&fd) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_sync(-1) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_directory_sync(work) == MAELYS_SYS_OK);
    CHECK(maelys_sys_directory_sync(path) == MAELYS_SYS_ERR_OS);
    CHECK(errno == ENOTDIR);
    CHECK(path_in_work("absent", other, sizeof(other)));
    CHECK(maelys_sys_directory_sync(other) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_directory_sync(NULL) == MAELYS_SYS_ERR_ARGUMENT);
    return 0;
}

static int test_publish(void) {
    char staging[512], destination[512], other[512];
    struct stat status;
    maelys_sys_publish_options_t options = {.sync_parent = 1};
    CHECK(path_in_work("stage", staging, sizeof(staging)));
    CHECK(path_in_work("final", destination, sizeof(destination)));
    CHECK(path_in_work("third", other, sizeof(other)));

    CHECK(write_plain("stage", "payload", 0600) == 0);
    CHECK(maelys_sys_file_publish_noreplace(staging, destination, &options) == MAELYS_SYS_OK);
    CHECK(lstat(destination, &status) == 0 && status.st_size == 7);
    CHECK(lstat(staging, &status) != 0 && errno == ENOENT);
    /* The destination is never replaced; the staged file stays. */
    CHECK(write_plain("stage", "other", 0600) == 0);
    CHECK(maelys_sys_file_publish_noreplace(staging, destination, NULL) ==
        MAELYS_SYS_ERR_EXISTS);
    CHECK(lstat(staging, &status) == 0 && status.st_size == 5);
    CHECK(lstat(destination, &status) == 0 && status.st_size == 7);
    /* Same file under both names. */
    CHECK(maelys_sys_file_publish_noreplace(destination, destination, NULL) ==
        MAELYS_SYS_ERR_EXISTS);
    /* A staged symbolic link or directory is refused, not interpreted. */
    CHECK(symlink(destination, other) == 0);
    CHECK(maelys_sys_file_publish_noreplace(other, staging, NULL) == MAELYS_SYS_ERR_IDENTITY);
    CHECK(unlink(other) == 0);
    CHECK(maelys_sys_file_publish_noreplace(work, other, NULL) == MAELYS_SYS_ERR_IDENTITY);
    /* A destination that is a dangling link still exists. */
    CHECK(symlink("nowhere", other) == 0);
    CHECK(maelys_sys_file_publish_noreplace(staging, other, NULL) == MAELYS_SYS_ERR_EXISTS);
    CHECK(unlink(other) == 0);
    CHECK(maelys_sys_file_publish_noreplace(other, staging, NULL) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_file_publish_noreplace(NULL, staging, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(unlink(staging) == 0 && unlink(destination) == 0);

    /* Directories. */
    CHECK(mkdir(staging, 0700) == 0);
    CHECK(maelys_sys_directory_publish_noreplace(staging, destination, &options) ==
        MAELYS_SYS_OK);
    CHECK(lstat(destination, &status) == 0 && S_ISDIR(status.st_mode));
    CHECK(mkdir(staging, 0700) == 0);
    CHECK(maelys_sys_directory_publish_noreplace(staging, destination, NULL) ==
        MAELYS_SYS_ERR_EXISTS);
    CHECK(lstat(staging, &status) == 0 && S_ISDIR(status.st_mode));
    /* A destination inside the staged directory: refused, nothing moved. */
    CHECK(path_in_work("stage/inside", other, sizeof(other)));
    CHECK(maelys_sys_directory_publish_noreplace(staging, other, NULL) ==
        MAELYS_SYS_ERR_UNSUPPORTED);
    CHECK(lstat(staging, &status) == 0 && lstat(other, &status) != 0);
    /* A staged file where a directory is expected. */
    CHECK(write_plain("plainfile", "x", 0600) == 0);
    CHECK(path_in_work("plainfile", other, sizeof(other)));
    CHECK(maelys_sys_directory_publish_noreplace(other, staging, NULL) ==
        MAELYS_SYS_ERR_IDENTITY);
    CHECK(unlink(other) == 0);
    CHECK(rmdir(staging) == 0 && rmdir(destination) == 0);
    return 0;
}

static int test_lock(void) {
    char path[512], other[512];
    maelys_sys_file_lock_t *first = NULL;
    maelys_sys_file_lock_t *second = NULL;
    maelys_sys_file_lock_options_t options = {
        .exclusive = 1, .wait = 0, .create = 1, .writable = 0, .expectations = NULL
    };
    struct stat status;
    CHECK(path_in_work("lock", path, sizeof(path)));

    /* Created on demand, 0600, and held exclusively. */
    CHECK(maelys_sys_file_lock_acquire(path, NULL, &first) == MAELYS_SYS_OK);
    CHECK(first != NULL && maelys_sys_file_lock_fd(first) >= 0);
    CHECK(lstat(path, &status) == 0 && (status.st_mode & 0777) == 0600);
    /* A second open file description in this process is "elsewhere". */
    CHECK(maelys_sys_file_lock_acquire(path, &options, &second) == MAELYS_SYS_ERR_BUSY);
    CHECK(second == NULL);
    CHECK(maelys_sys_file_lock_release(&first) == MAELYS_SYS_OK);
    CHECK(first == NULL);
    CHECK(maelys_sys_file_lock_release(&first) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_lock_fd(NULL) == -1);

    /* Shared locks coexist; an exclusive one waits or refuses. */
    options.exclusive = 0;
    CHECK(maelys_sys_file_lock_acquire(path, &options, &first) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_lock_acquire(path, &options, &second) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_lock_release(&second) == MAELYS_SYS_OK);
    options.exclusive = 1;
    CHECK(maelys_sys_file_lock_acquire(path, &options, &second) == MAELYS_SYS_ERR_BUSY);
    CHECK(maelys_sys_file_lock_release(&first) == MAELYS_SYS_OK);

    /* The lock file is never removed by release. */
    CHECK(lstat(path, &status) == 0);
    /* Default expectations refuse a file others can read. */
    CHECK(chmod(path, 0644) == 0);
    CHECK(maelys_sys_file_lock_acquire(path, &options, &first) == MAELYS_SYS_ERR_IDENTITY);
    CHECK(first == NULL);
    CHECK(chmod(path, 0400) == 0);
    /* Read-only by default: a 0400 file locks; writable refuses it. */
    CHECK(maelys_sys_file_lock_acquire(path, &options, &first) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_lock_release(&first) == MAELYS_SYS_OK);
    options.writable = 1;
    if (geteuid() != 0) {
        /* Root opens anything read-write; the refusal only shows otherwise. */
        CHECK(maelys_sys_file_lock_acquire(path, &options, &first) == MAELYS_SYS_ERR_OS);
        CHECK(errno == EACCES);
    } else {
        CHECK(maelys_sys_file_lock_acquire(path, &options, &first) == MAELYS_SYS_OK);
        CHECK(maelys_sys_file_lock_release(&first) == MAELYS_SYS_OK);
    }
    options.writable = 0;
    CHECK(chmod(path, 0600) == 0);

    /* create off: missing is not found; a symbolic link is never followed. */
    options.create = 0;
    CHECK(path_in_work("lock-missing", other, sizeof(other)));
    CHECK(maelys_sys_file_lock_acquire(other, &options, &first) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(symlink(path, other) == 0);
    CHECK(maelys_sys_file_lock_acquire(other, &options, &first) == MAELYS_SYS_ERR_IDENTITY);
    options.create = 1;
    CHECK(maelys_sys_file_lock_acquire(other, &options, &first) == MAELYS_SYS_ERR_IDENTITY);
    CHECK(lstat(other, &status) == 0 && S_ISLNK(status.st_mode));
    CHECK(unlink(other) == 0);
    /* A directory at the path. */
    CHECK(maelys_sys_file_lock_acquire(work, &options, &first) == MAELYS_SYS_ERR_IDENTITY);
    CHECK(maelys_sys_file_lock_acquire(NULL, &options, &first) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_file_lock_acquire(path, &options, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(unlink(path) == 0);
    return 0;
}

static int remove_work(void) {
    char path[512];
    static const char *names[] = {"plain", "bounded", "written", "empty"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (path_in_work(names[i], path, sizeof(path))) (void)unlink(path);
    }
    return rmdir(work);
}

int main(void) {
    const char *base = getenv("TMPDIR");
    if (!base || base[0] != '/') base = "/tmp";
    int written = snprintf(work, sizeof(work), "%s/maelys-sys-file.XXXXXX", base);
    if (written <= 0 || (size_t)written >= sizeof(work) || !mkdtemp(work)) return 1;
    int failed = test_verify_and_open() || test_read_bounded() ||
        test_write_exclusive_and_sync() || test_publish() || test_lock();
    if (remove_work() != 0 && !failed) {
        fprintf(stderr, "work directory not empty: %s\n", work);
        return 1;
    }
    if (failed) return 1;
    puts("ok - file verify, open and identity");
    puts("ok - file bounded read");
    puts("ok - file exclusive write and sync");
    puts("ok - file and directory no-replace publication");
    puts("ok - file lock");
    return 0;
}
