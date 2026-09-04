/*
 * White-box fault injection for the file primitives: src/file.c is compiled
 * into this unit with a fault point before each system call the contracts
 * speak about, so each failure path is executed and its promise checked: nothing half-written
 * is left behind, nothing moved when a publication fails, no lock held
 * when the identity check after the lock fails.
 */
#define MAELYS_SYS_FILE_TESTING 1
#include "src/file.c"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s (errno %d: %s)\n", \
            __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return 1; \
    } \
} while (0)

/* The step to fail, its errno, how many matching steps to let through
 * first, and an optional action to run at the step instead of failing. */
static const char *fault_step;
static const char *fault_next;   /* armed with the same errno once fault_step fired */
static int fault_errno;
static int fault_skip;
static int (*fault_action)(void);

static int fault_next_errno;

static int file_fault(const char *step) {
    if (!fault_step || strcmp(step, fault_step) != 0) return 0;
    if (fault_skip > 0) { --fault_skip; return 0; }
    int error = fault_errno;
    fault_step = fault_next;
    fault_next = NULL;
    if (fault_step) fault_errno = fault_next_errno;
    if (fault_action) {
        int (*action)(void) = fault_action;
        fault_action = NULL;
        return action();
    }
    errno = error;
    return 1;
}

static void arm(const char *step, int error, int skip) {
    fault_step = step;
    fault_next = NULL;
    fault_errno = error;
    fault_skip = skip;
    fault_action = NULL;
}

/* Fails the durability step: on macOS F_FULLFSYNC is tried first and
 * fsync(2) is its fallback, so both must fail for the call to fail. */
static void arm_sync(int error) {
#if defined(__APPLE__)
    /* A refused F_FULLFSYNC falls back to fsync(2); the error of the test
     * is then fsync's. */
    arm("fullfsync", ENOTSUP, 0);
    fault_next = "fsync";
    fault_next_errno = error;
#else
    arm("fsync", error, 0);
#endif
}

static char work[256];
static char lock_path[512];
static char replacement_path[512];

static int swap_written_file(void);

static int join(const char *name, char *out, size_t capacity) {
    int written = snprintf(out, capacity, "%s/%s", work, name);
    return written > 0 && (size_t)written < capacity;
}

static int absent(const char *path) {
    struct stat status;
    return lstat(path, &status) != 0 && errno == ENOENT;
}

static mode_t mode_while_writing;

static int check_mode_while_writing(void) {
    char path[512];
    struct stat status;
    if (!join("exclusive", path, sizeof(path)) || lstat(path, &status) != 0) return -1;
    mode_while_writing = status.st_mode & 0777;
    return 0;
}

static int write_exclusive_failures(void) {
    char path[512];
    CHECK(join("exclusive", path, sizeof(path)));
    static const struct { const char *step; int error; } faults[] = {
        {"open", EACCES}, {"fstat", EIO}, {"write", ENOSPC},
        {"fchmod", EPERM}, {"sync", EIO}, {"close", EIO}
    };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        if (strcmp(faults[i].step, "sync") == 0) arm_sync(faults[i].error);
        else arm(faults[i].step, faults[i].error, 0);
        errno = 0;
        maelys_sys_result_t result =
            maelys_sys_file_write_exclusive(path, "payload", 7u, 0644);
        CHECK(result == MAELYS_SYS_ERR_OS);
        CHECK(errno == faults[i].error);
        /* Nothing half-written survives. */
        CHECK(absent(path));
    }
#if defined(__APPLE__)
    /* F_FULLFSYNC refused: fsync(2) is the fallback and the write succeeds. */
    arm("fullfsync", ENOTSUP, 0);
    CHECK(maelys_sys_file_write_exclusive(path, "payload", 7u, 0644) == MAELYS_SYS_OK);
    CHECK(unlink(path) == 0);
    /* F_FULLFSYNC failing with an I/O error is a lost write, not a refusal. */
    arm("fullfsync", EIO, 0);
    errno = 0;
    CHECK(maelys_sys_file_write_exclusive(path, "payload", 7u, 0644) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EIO && absent(path));
#endif
    /* While the content is written the file is 0600, whatever the final
     * mode; it receives the final mode only afterwards. Under a permissive
     * umask, so that a file created 0644 would show as such. */
    {
        mode_t previous = umask(022);
        arm("write", 0, 0);
        fault_action = check_mode_while_writing;
        CHECK(maelys_sys_file_write_exclusive(path, "payload", 7u, 0644) == MAELYS_SYS_OK);
        (void)umask(previous);
        CHECK(mode_while_writing == 0600);
        struct stat final_status;
        CHECK(lstat(path, &final_status) == 0 && (final_status.st_mode & 0777) == 0644);
        CHECK(unlink(path) == 0);
    }
    /* The removal on failure targets the created file only: when the path
     * was swapped meanwhile, the newcomer is left alone. */
    static char swapped_in[512];
    CHECK(join("swapped", swapped_in, sizeof(swapped_in)));
    CHECK(maelys_sys_file_write_exclusive(swapped_in, "other", 5u, 0600) == MAELYS_SYS_OK);
    {
        static char target[512];
        CHECK(join("exclusive", target, sizeof(target)));
        /* At fchmod time, move another file over the path being written. */
        arm("fchmod", 0, 0);
        fault_action = swap_written_file;
        CHECK(maelys_sys_file_write_exclusive(target, "payload", 7u, 0644) == MAELYS_SYS_OK);
        /* swap returned 0 after renaming: the write finished on the old
         * inode, which no longer has a name; the swapped file stays. */
        struct stat status;
        CHECK(lstat(target, &status) == 0 && status.st_size == 5);
        CHECK(unlink(target) == 0);
    }
    return 0;
}

static int swap_written_file(void) {
    char target[512];
    if (!join("exclusive", target, sizeof(target))) return -1;
    char source[512];
    if (!join("swapped", source, sizeof(source))) return -1;
    return rename(source, target) == 0 ? 0 : -1;
}

static int publish_failures(void) {
    char staging[512], destination[512];
    struct stat status;
    CHECK(join("stage", staging, sizeof(staging)));
    CHECK(join("final", destination, sizeof(destination)));
    CHECK(maelys_sys_file_write_exclusive(staging, "p", 1u, 0600) == MAELYS_SYS_OK);
    /* The rename fails: nothing moved. */
    static const struct { int error; maelys_sys_result_t expected; } faults[] = {
        {EIO, MAELYS_SYS_ERR_OS}, {EINVAL, MAELYS_SYS_ERR_UNSUPPORTED},
        {ENOTSUP, MAELYS_SYS_ERR_UNSUPPORTED}, {EEXIST, MAELYS_SYS_ERR_EXISTS}
    };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        arm("rename", faults[i].error, 0);
        CHECK(maelys_sys_file_publish_noreplace(staging, destination, NULL) ==
            faults[i].expected);
        CHECK(lstat(staging, &status) == 0 && absent(destination));
    }
    /* The parent sync fails after the rename: the file is published and the
     * failure reported, so the caller knows the entry may not be durable. */
    arm_sync(EIO);
    maelys_sys_publish_options_t options = {.sync_parent = 1};
    errno = 0;
    maelys_sys_result_t result =
        maelys_sys_file_publish_noreplace(staging, destination, &options);
    CHECK(result == MAELYS_SYS_ERR_OS && errno == EIO);
    CHECK(lstat(destination, &status) == 0 && absent(staging));
    CHECK(unlink(destination) == 0);
    /* lstat of the staging fails as an OS error, not "not found". */
    CHECK(maelys_sys_file_write_exclusive(staging, "p", 1u, 0600) == MAELYS_SYS_OK);
    arm("lstat", EACCES, 0);
    CHECK(maelys_sys_file_publish_noreplace(staging, destination, NULL) == MAELYS_SYS_ERR_OS);
    CHECK(unlink(staging) == 0);
    return 0;
}

static char keepalive_path[512];

static int replace_lock_file(void) {
    /* Between open and flock: another file takes the lock path. */
    return rename(replacement_path, lock_path) == 0 ? 0 : -1;
}

static int loosen_lock_file(void) {
    /* Between open and flock: the file becomes readable by others. */
    return chmod(lock_path, 0644);
}

static int lock_failures(void) {
    maelys_sys_file_lock_t *lock = NULL;
    struct stat status;
    CHECK(join("lock", lock_path, sizeof(lock_path)));
    CHECK(join("lock-replacement", replacement_path, sizeof(replacement_path)));

    /* Replaced between open and flock while the old inode stays alive
     * through another name: only the path check sees it. */
    CHECK(maelys_sys_file_write_exclusive(lock_path, "", 0u, 0600) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_write_exclusive(replacement_path, "", 0u, 0600) == MAELYS_SYS_OK);
    CHECK(join("lock-keepalive", keepalive_path, sizeof(keepalive_path)));
    CHECK(link(lock_path, keepalive_path) == 0);
    arm("flock", 0, 0);
    fault_action = replace_lock_file;
    maelys_sys_file_expectations_t any_links = {0};
    maelys_sys_file_lock_options_t loose = {
        .exclusive = 1, .wait = 1, .create = 1, .writable = 0, .expectations = &any_links
    };
    CHECK(maelys_sys_file_lock_acquire(lock_path, &loose, &lock) == MAELYS_SYS_ERR_IDENTITY);
    CHECK(lock == NULL);
    CHECK(unlink(keepalive_path) == 0);
    /* Changed between open and flock: only the second verification sees it. */
    arm("flock", 0, 0);
    fault_action = loosen_lock_file;
    CHECK(maelys_sys_file_lock_acquire(lock_path, NULL, &lock) == MAELYS_SYS_ERR_IDENTITY);
    CHECK(lock == NULL);
    CHECK(chmod(lock_path, 0600) == 0);
    /* The replacement is lockable by a fresh caller: nothing lingers. */
    maelys_sys_file_lock_options_t nowait = {.exclusive = 1, .wait = 0, .create = 0};
    CHECK(maelys_sys_file_lock_acquire(lock_path, &nowait, &lock) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_lock_release(&lock) == MAELYS_SYS_OK);

    /* Each system call failing in turn. */
    arm("open", EACCES, 0);
    CHECK(maelys_sys_file_lock_acquire(lock_path, NULL, &lock) == MAELYS_SYS_ERR_OS);
    arm("fcntl", EBADF, 0);
    CHECK(maelys_sys_file_lock_acquire(lock_path, NULL, &lock) == MAELYS_SYS_ERR_OS);
    arm("fstat", EIO, 0);
    CHECK(maelys_sys_file_lock_acquire(lock_path, NULL, &lock) == MAELYS_SYS_ERR_OS);
    arm("flock", ENOLCK, 0);
    CHECK(maelys_sys_file_lock_acquire(lock_path, NULL, &lock) == MAELYS_SYS_ERR_OS);
    arm("fstat", EIO, 1);   /* the verification after the lock */
    errno = 0;
    CHECK(maelys_sys_file_lock_acquire(lock_path, NULL, &lock) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EIO);
    arm("lstat", EACCES, 0);
    errno = 0;
    CHECK(maelys_sys_file_lock_acquire(lock_path, NULL, &lock) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EACCES);
    arm("lstat", ENOENT, 0); /* the path vanished: not the caller's file */
    CHECK(maelys_sys_file_lock_acquire(lock_path, NULL, &lock) == MAELYS_SYS_ERR_IDENTITY);
    CHECK(lock == NULL);
    /* After every failure the lock is free. */
    CHECK(maelys_sys_file_lock_acquire(lock_path, &nowait, &lock) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_lock_release(&lock) == MAELYS_SYS_OK);
    CHECK(lstat(lock_path, &status) == 0);
    CHECK(unlink(lock_path) == 0);
    return 0;
}

static int read_and_open_failures(void) {
    char path[512];
    unsigned char buffer[4];
    size_t size = 0;
    int fd = -1;
    CHECK(join("readable", path, sizeof(path)));
    CHECK(maelys_sys_file_write_exclusive(path, "abcd", 4u, 0600) == MAELYS_SYS_OK);
    arm("open", EACCES, 0);
    CHECK(maelys_sys_file_open_trusted(path, NULL, NULL, NULL, &fd) == MAELYS_SYS_ERR_OS);
    CHECK(fd == -1);
    arm("fcntl", EBADF, 0);
    CHECK(maelys_sys_file_open_trusted(path, NULL, NULL, NULL, &fd) == MAELYS_SYS_ERR_OS);
    arm("fstat", EIO, 0);
    CHECK(maelys_sys_file_open_trusted(path, NULL, NULL, NULL, &fd) == MAELYS_SYS_ERR_OS);
    CHECK(maelys_sys_file_open_trusted(path, NULL, NULL, NULL, &fd) == MAELYS_SYS_OK);
    arm("read", EIO, 0);
    CHECK(maelys_sys_file_read_bounded(fd, buffer, 4u, &size) == MAELYS_SYS_ERR_OS);
    CHECK(size == 0u);
    CHECK(maelys_sys_fd_close(&fd) == MAELYS_SYS_OK);
    arm("open", EIO, 0);
    CHECK(maelys_sys_directory_sync(work) == MAELYS_SYS_ERR_OS);
    arm("lstat", EIO, 0);
    maelys_sys_file_identity_t identity;
    CHECK(maelys_sys_file_path_identity(path, &identity) == MAELYS_SYS_ERR_OS);
    CHECK(unlink(path) == 0);
    return 0;
}

int main(void) {
    const char *base = getenv("TMPDIR");
    if (!base || base[0] != '/') base = "/tmp";
    int written = snprintf(work, sizeof(work), "%s/maelys-sys-faults.XXXXXX", base);
    if (written <= 0 || (size_t)written >= sizeof(work) || !mkdtemp(work)) return 1;
    int failed = write_exclusive_failures() || publish_failures() ||
        lock_failures() || read_and_open_failures();
    if (!failed && rmdir(work) != 0) {
        fprintf(stderr, "work directory not empty: %s\n", work);
        return 1;
    }
    if (failed) return 1;
    puts("ok - exclusive write fails clean at every step");
    puts("ok - publication moves nothing on failure");
    puts("ok - lock holds nothing after a failed identity check");
    puts("ok - open, read and sync report their failing call");
    return 0;
}
