/*
 * Directory watching, white box: src/dirwatch.c is compiled into this unit
 * with a fault point before the system calls its failure paths depend on,
 * and one that stands for the kernel dropping events. Only inotify drops
 * events by itself, and only with a queue no test may shrink without
 * privilege: through the fault, both hosts run the same OVERFLOW path.
 */
#define MAELYS_SYS_DIRWATCH_TESTING 1
#include "src/dirwatch.c"

#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s (errno %d: %s)\n", \
            __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return 1; \
    } \
} while (0)

#define PATH_ROOM 512

static const char *fault_step;
static int fault_errno;

static int dirwatch_fault(const char *step) {
    if (!fault_step || strcmp(step, fault_step) != 0) return 0;
    fault_step = NULL;
    errno = fault_errno;
    return 1;
}

static void arm(const char *step, int error) {
    fault_step = step;
    fault_errno = error;
}

static char root[256];

static int in(const char *base, const char *name, char *out) {
    int written = snprintf(out, PATH_ROOM, "%s/%s", base, name);
    return written > 0 && written < PATH_ROOM;
}

static int count_open(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

/* The kernel dropped events: every live entry says CHANGED|OVERFLOW, once. */
static int test_overflow(void) {
    char dirs[3][PATH_ROOM];
    static const char *names[3] = {"over-a", "over-b", "over-c"};
    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_dirwatch_entry_t entries[4] = {0, 0, 0, 0};
    CHECK(maelys_sys_dirwatch_create(4u, &dirwatch) == MAELYS_SYS_OK);
    for (size_t index = 0; index < 3u; ++index) {
        CHECK(in(root, names[index], dirs[index]) && mkdir(dirs[index], 0700) == 0);
        CHECK(maelys_sys_dirwatch_add(dirwatch, dirs[index], 10u + index,
            &entries[index]) == MAELYS_SYS_OK);
    }
    arm("overflow", 0);
    maelys_sys_dirwatch_change_t changes[8];
    size_t count = 0;
    CHECK(maelys_sys_dirwatch_poll(dirwatch, changes, 8u, &count) == MAELYS_SYS_OK);
    CHECK(count == 3u && fault_step == NULL);
    unsigned seen = 0;
    for (size_t position = 0; position < count; ++position) {
        CHECK(changes[position].flags ==
            (MAELYS_SYS_DIRWATCH_CHANGED | MAELYS_SYS_DIRWATCH_OVERFLOW));
        for (unsigned index = 0; index < 3u; ++index) {
            if (changes[position].entry == entries[index]) {
                CHECK(changes[position].token == 10u + index);
                seen |= 1u << index;
            }
        }
    }
    CHECK(seen == 7u);
    CHECK(maelys_sys_dirwatch_poll(dirwatch, changes, 8u, &count) ==
        MAELYS_SYS_ERR_WOULD_BLOCK && count == 0u);
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    for (size_t index = 0; index < 3u; ++index) CHECK(rmdir(dirs[index]) == 0);
    return 0;
}

/* The kernel refusing the handle, the registration or the read: each is
 * reported with its errno and leaves nothing open nor half-made. */
static int test_kernel_refusals(void) {
    char dir[PATH_ROOM], file[PATH_ROOM];
    CHECK(in(root, "refused", dir) && mkdir(dir, 0700) == 0 && in(dir, "f", file));
    int before = count_open();
    maelys_sys_dirwatch_t *dirwatch = (maelys_sys_dirwatch_t *)&root;
    arm("create", EMFILE);
    errno = 0;
    CHECK(maelys_sys_dirwatch_create(2u, &dirwatch) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EMFILE && dirwatch == NULL && count_open() == before);
    /* A kernel without the facility is not an error of the caller's. */
    arm("create", ENOSYS);
    CHECK(maelys_sys_dirwatch_create(2u, &dirwatch) == MAELYS_SYS_ERR_UNSUPPORTED);
    CHECK(dirwatch == NULL && count_open() == before);

    CHECK(maelys_sys_dirwatch_create(2u, &dirwatch) == MAELYS_SYS_OK);
    int with_handle = count_open();
    maelys_sys_dirwatch_entry_t entry = 9;
    arm("register", ENOSPC);
    errno = 0;
    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 1, &entry) == MAELYS_SYS_ERR_OS);
    CHECK(errno == ENOSPC && entry == 0u && count_open() == with_handle);
    /* The refused add left the place free and the directory unwatched. */
    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 1, &entry) == MAELYS_SYS_OK && entry != 0u);
    int fd = open(file, O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    maelys_sys_dirwatch_change_t change;
    size_t count = 9;
    arm("read", EIO);
    errno = 0;
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) == MAELYS_SYS_ERR_OS);
    CHECK(errno == EIO && count == 0u);
    /* What the failed read did not take is still there for the next. */
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) == MAELYS_SYS_OK);
    CHECK(count == 1u && change.entry == entry &&
        change.flags == MAELYS_SYS_DIRWATCH_CHANGED);
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    CHECK(count_open() == before);
    CHECK(unlink(file) == 0 && rmdir(dir) == 0);
    return 0;
}

int main(void) {
    const char *base = getenv("TMPDIR");
    if (!base || base[0] != '/') base = "/tmp";
    int written = snprintf(root, sizeof(root), "%s/maelys-sys-dirwatch-faults.XXXXXX", base);
    if (written <= 0 || (size_t)written >= sizeof(root) || !mkdtemp(root)) return 1;
    if (test_overflow() || test_kernel_refusals()) return 1;
    if (rmdir(root) != 0) {
        fprintf(stderr, "work directory not empty: %s\n", root);
        return 1;
    }
    puts("ok - dirwatch overflow marks every live entry once");
    puts("ok - dirwatch kernel refusals reported, nothing left open");
    return 0;
}
