/*
 * Directory watching, black box, in private temporary directories. Every
 * expectation below holds on Linux and macOS alike: that is the contract.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE /* mkdtemp, setrlimit under strict POSIX */
#else
#define _DEFAULT_SOURCE
#endif

#include "maelys/sys.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s (errno %d: %s)\n", \
            __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return 1; \
    } \
} while (0)

#define PATH_ROOM 512
#define CHANGED MAELYS_SYS_DIRWATCH_CHANGED
#define GONE MAELYS_SYS_DIRWATCH_GONE

static char root[256];

static int in(const char *base, const char *name, char *out) {
    int written = snprintf(out, PATH_ROOM, "%s/%s", base, name);
    return written > 0 && written < PATH_ROOM;
}

static int put(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    size_t length = strlen(text);
    int ok = write(fd, text, length) == (ssize_t)length;
    return close(fd) == 0 && ok ? 0 : -1;
}

static int count_open(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

/* Waits for the descriptor to say something, then polls once. */
static maelys_sys_result_t next(
    maelys_sys_dirwatch_t *dirwatch,
    maelys_sys_dirwatch_change_t *changes,
    size_t capacity,
    size_t *count) {
    uint64_t deadline = 0;
    unsigned flags = 0;
    if (maelys_sys_deadline_after(3000, &deadline) != MAELYS_SYS_OK) return MAELYS_SYS_ERR_OS;
    maelys_sys_result_t ready = maelys_sys_fd_wait(maelys_sys_dirwatch_fd(dirwatch),
        MAELYS_SYS_INTEREST_READ, deadline, &flags);
    if (ready != MAELYS_SYS_OK) return ready;
    return maelys_sys_dirwatch_poll(dirwatch, changes, capacity, count);
}

/* Nothing comes: the descriptor stays quiet and a poll has nothing. */
static int quiet(maelys_sys_dirwatch_t *dirwatch) {
    uint64_t deadline = 0;
    unsigned flags = 0;
    maelys_sys_dirwatch_change_t change;
    size_t count = 7;
    if (maelys_sys_deadline_after(150, &deadline) != MAELYS_SYS_OK) return 0;
    if (maelys_sys_fd_wait(maelys_sys_dirwatch_fd(dirwatch), MAELYS_SYS_INTEREST_READ,
            deadline, &flags) != MAELYS_SYS_ERR_TIMEOUT) {
        return 0;
    }
    return maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) ==
        MAELYS_SYS_ERR_WOULD_BLOCK && count == 0u;
}

/* Nothing to report, whatever the descriptor says: releasing an entry can
 * leave it readable on Linux, where the kernel acknowledges the release. */
static int nothing_pending(maelys_sys_dirwatch_t *dirwatch) {
    maelys_sys_dirwatch_change_t change;
    size_t count = 7;
    return maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) ==
        MAELYS_SYS_ERR_WOULD_BLOCK && count == 0u;
}

/* One CHANGED for this entry and token, and nothing after it. */
static int changed_once(
    maelys_sys_dirwatch_t *dirwatch,
    maelys_sys_dirwatch_entry_t entry,
    maelys_sys_token_t token) {
    maelys_sys_dirwatch_change_t changes[4];
    size_t count = 0;
    if (next(dirwatch, changes, 4u, &count) != MAELYS_SYS_OK || count != 1u) return 0;
    if (changes[0].entry != entry || changes[0].token != token ||
        changes[0].flags != CHANGED) {
        return 0;
    }
    return maelys_sys_dirwatch_poll(dirwatch, changes, 4u, &count) ==
        MAELYS_SYS_ERR_WOULD_BLOCK;
}

static int test_arguments(void) {
    maelys_sys_dirwatch_t *dirwatch = (maelys_sys_dirwatch_t *)&root;
    maelys_sys_dirwatch_entry_t entry = 9;
    maelys_sys_dirwatch_change_t change;
    size_t count = 9;
    CHECK(maelys_sys_dirwatch_create(0u, &dirwatch) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(dirwatch == NULL);
    CHECK(maelys_sys_dirwatch_create(4u, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_dirwatch_fd(NULL) == -1);
    CHECK(maelys_sys_dirwatch_destroy(NULL) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_create(4u, &dirwatch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_fd(dirwatch) >= 0);
    CHECK(maelys_sys_dirwatch_add(NULL, root, 1, &entry) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(entry == 0u);
    entry = 9;
    CHECK(maelys_sys_dirwatch_add(dirwatch, NULL, 1, &entry) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(entry == 0u);
    CHECK(maelys_sys_dirwatch_add(dirwatch, root, 1, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_dirwatch_remove(NULL, 1u) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_dirwatch_remove(dirwatch, 0u) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_dirwatch_remove(dirwatch, 12345u) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_dirwatch_poll(NULL, &change, 1u, &count) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(count == 0u);
    CHECK(maelys_sys_dirwatch_poll(dirwatch, NULL, 1u, &count) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 0u, &count) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, NULL) == MAELYS_SYS_ERR_ARGUMENT);
    /* Nothing watched, nothing pending: the normal state. */
    count = 9;
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) ==
        MAELYS_SYS_ERR_WOULD_BLOCK && count == 0u);
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK && dirwatch == NULL);
    return 0;
}

/* What the contract promises to report, on both hosts. */
static int test_entries_reported(void) {
    char dir[PATH_ROOM], aside[PATH_ROOM], a[PATH_ROOM], b[PATH_ROOM], staged[PATH_ROOM];
    CHECK(in(root, "reported", dir) && in(root, "reported-aside", aside));
    CHECK(mkdir(dir, 0700) == 0 && mkdir(aside, 0700) == 0);
    CHECK(in(dir, "a", a) && in(dir, "b", b) && in(aside, "staged", staged));
    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_dirwatch_entry_t entry = 0;
    CHECK(maelys_sys_dirwatch_create(4u, &dirwatch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 42, &entry) == MAELYS_SYS_OK && entry != 0u);
    CHECK(quiet(dirwatch));
    /* Add, then read: what happens in between is reported like the rest. */
    CHECK(put(a, "one") == 0);
    CHECK(changed_once(dirwatch, entry, 42));
    CHECK(rename(a, b) == 0);
    CHECK(changed_once(dirwatch, entry, 42));
    CHECK(unlink(b) == 0);
    CHECK(changed_once(dirwatch, entry, 42));
    /* A reload of configuration: a member replaced by rename onto its name,
     * staged in the directory, then staged elsewhere. */
    CHECK(put(a, "old") == 0);
    CHECK(changed_once(dirwatch, entry, 42));
    CHECK(put(b, "new") == 0 && rename(b, a) == 0);
    CHECK(changed_once(dirwatch, entry, 42));
    CHECK(put(staged, "newer") == 0 && rename(staged, a) == 0);
    CHECK(changed_once(dirwatch, entry, 42));
    /* The library's own publication. */
    CHECK(maelys_sys_file_write_exclusive(staged, "p", 1u, 0600) == MAELYS_SYS_OK);
    CHECK(maelys_sys_file_publish_noreplace(staged, b, NULL) == MAELYS_SYS_OK);
    CHECK(changed_once(dirwatch, entry, 42));
    /* A subdirectory appearing is a change of the entries. */
    char sub[PATH_ROOM], inner[PATH_ROOM];
    CHECK(in(dir, "sub", sub) && in(sub, "inner", inner));
    CHECK(mkdir(sub, 0700) == 0);
    CHECK(changed_once(dirwatch, entry, 42));
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    CHECK(unlink(a) == 0 && unlink(b) == 0 && rmdir(sub) == 0);
    CHECK(rmdir(dir) == 0 && rmdir(aside) == 0);
    return 0;
}

/* What is not reported, on either host, and that the entry lives on. */
static int test_not_reported(void) {
    char dir[PATH_ROOM], a[PATH_ROOM], sub[PATH_ROOM], inner[PATH_ROOM];
    CHECK(in(root, "silent", dir) && mkdir(dir, 0700) == 0);
    CHECK(in(dir, "a", a) && in(dir, "sub", sub) && in(sub, "inner", inner));
    CHECK(put(a, "one") == 0 && mkdir(sub, 0700) == 0);
    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_dirwatch_entry_t entry = 0;
    CHECK(maelys_sys_dirwatch_create(4u, &dirwatch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 5, &entry) == MAELYS_SYS_OK);
    /* A write in place: Linux could tell, and is not asked to. */
    int fd = open(a, O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, "more", 4) == 4 && close(fd) == 0);
    CHECK(quiet(dirwatch));
    /* Metadata of a member, and of the directory itself. */
    CHECK(chmod(a, 0644) == 0);
    CHECK(quiet(dirwatch));
    CHECK(chmod(dir, 0750) == 0);
    CHECK(quiet(dirwatch));
    /* The content of a subdirectory. */
    CHECK(put(inner, "x") == 0);
    CHECK(quiet(dirwatch));
    /* The entry still reports what it promises. */
    CHECK(unlink(a) == 0);
    CHECK(changed_once(dirwatch, entry, 5));
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    CHECK(unlink(inner) == 0 && rmdir(sub) == 0 && rmdir(dir) == 0);
    return 0;
}

/* A thousand events, one change; then entries served in turn. */
static int test_coalescing_and_turns(void) {
    char dirs[3][PATH_ROOM], file[PATH_ROOM];
    static const char *names[3] = {"turn-a", "turn-b", "turn-c"};
    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_dirwatch_entry_t entries[3] = {0, 0, 0};
    maelys_sys_dirwatch_change_t change;
    size_t count = 0;
    CHECK(maelys_sys_dirwatch_create(3u, &dirwatch) == MAELYS_SYS_OK);
    for (size_t index = 0; index < 3u; ++index) {
        CHECK(in(root, names[index], dirs[index]) && mkdir(dirs[index], 0700) == 0);
        CHECK(maelys_sys_dirwatch_add(dirwatch, dirs[index], 100u + index,
            &entries[index]) == MAELYS_SYS_OK);
    }
    CHECK(entries[0] != entries[1] && entries[1] != entries[2]);
    for (int round = 0; round < 1000; ++round) {
        char name[32];
        CHECK(snprintf(name, sizeof(name), "f%d", round) > 0 && in(dirs[0], name, file));
        CHECK(put(file, "") == 0 && unlink(file) == 0);
    }
    CHECK(changed_once(dirwatch, entries[0], 100));

    /* Three entries changed, an array of one: three calls, three entries. */
    for (size_t index = 0; index < 3u; ++index) {
        CHECK(in(dirs[index], "x", file) && put(file, "") == 0);
    }
    unsigned seen = 0;
    for (int call = 0; call < 3; ++call) {
        CHECK((call == 0 ? next(dirwatch, &change, 1u, &count) :
            maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count)) == MAELYS_SYS_OK);
        CHECK(count == 1u && change.flags == CHANGED);
        for (unsigned index = 0; index < 3u; ++index) {
            if (change.entry == entries[index]) {
                CHECK(change.token == 100u + index && !(seen & (1u << index)));
                seen |= 1u << index;
            }
        }
    }
    CHECK(seen == 7u);
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) ==
        MAELYS_SYS_ERR_WOULD_BLOCK);

    /* Turns: the entry just served changes again, yet the others come first. */
    for (size_t index = 0; index < 3u; ++index) {
        CHECK(in(dirs[index], "y", file) && put(file, "") == 0);
    }
    CHECK(next(dirwatch, &change, 1u, &count) == MAELYS_SYS_OK && count == 1u);
    maelys_sys_dirwatch_entry_t first = change.entry;
    size_t first_index = first == entries[0] ? 0u : first == entries[1] ? 1u : 2u;
    CHECK(in(dirs[first_index], "z", file) && put(file, "") == 0);
    /* The descriptor speaks for the new event only; what is still owed from
     * before is asked for by polling on. */
    CHECK(next(dirwatch, &change, 1u, &count) == MAELYS_SYS_OK && count == 1u);
    CHECK(change.entry != first);
    maelys_sys_dirwatch_entry_t second = change.entry;
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) == MAELYS_SYS_OK);
    CHECK(count == 1u && change.entry != first && change.entry != second);
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) == MAELYS_SYS_OK);
    CHECK(count == 1u && change.entry == first);
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) ==
        MAELYS_SYS_ERR_WOULD_BLOCK);

    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    for (size_t index = 0; index < 3u; ++index) {
        CHECK(in(dirs[index], "x", file) && unlink(file) == 0);
        CHECK(in(dirs[index], "y", file) && unlink(file) == 0);
        CHECK(in(dirs[index], "z", file) && (unlink(file) == 0 || errno == ENOENT));
        CHECK(rmdir(dirs[index]) == 0);
    }
    return 0;
}

/* The directory itself leaving: removed, or renamed. */
static int test_gone(void) {
    char removed[PATH_ROOM], renamed[PATH_ROOM], elsewhere[PATH_ROOM];
    CHECK(in(root, "gone-removed", removed) && in(root, "gone-renamed", renamed));
    CHECK(in(root, "gone-elsewhere", elsewhere));
    CHECK(mkdir(removed, 0700) == 0 && mkdir(renamed, 0700) == 0);
    int before = count_open();
    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_dirwatch_entry_t first = 0, second = 0, again = 0;
    maelys_sys_dirwatch_change_t change;
    size_t count = 0;
    CHECK(maelys_sys_dirwatch_create(2u, &dirwatch) == MAELYS_SYS_OK);
    int with_handle = count_open();
    CHECK(maelys_sys_dirwatch_add(dirwatch, removed, 1, &first) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_add(dirwatch, renamed, 2, &second) == MAELYS_SYS_OK);
    CHECK(rmdir(removed) == 0 && rename(renamed, elsewhere) == 0);
    /* An array of one: a GONE that does not fit keeps its entry live. */
    CHECK(next(dirwatch, &change, 1u, &count) == MAELYS_SYS_OK && count == 1u);
    CHECK(change.flags & GONE);
    maelys_sys_dirwatch_entry_t delivered = change.entry;
    maelys_sys_dirwatch_entry_t waiting = delivered == first ? second : first;
    CHECK(delivered == first || delivered == second);
    CHECK(change.token == (delivered == first ? 1u : 2u));
    /* Delivered: released, and its number is never live again. */
    CHECK(maelys_sys_dirwatch_remove(dirwatch, delivered) == MAELYS_SYS_ERR_NOT_FOUND);
    /* Not delivered yet: the table is still full of it. */
    CHECK(mkdir(removed, 0700) == 0);
    CHECK(maelys_sys_dirwatch_add(dirwatch, removed, 3, &again) == MAELYS_SYS_OK);
    CHECK(again != first && again != second && again > second);
    char probe[PATH_ROOM];
    CHECK(in(root, "gone-probe", probe) && mkdir(probe, 0700) == 0);
    maelys_sys_dirwatch_entry_t none = 9;
    CHECK(maelys_sys_dirwatch_add(dirwatch, probe, 4, &none) == MAELYS_SYS_ERR_CAPACITY);
    CHECK(none == 0u);
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) == MAELYS_SYS_OK);
    CHECK(count == 1u && change.entry == waiting && (change.flags & GONE));
    CHECK(maelys_sys_dirwatch_remove(dirwatch, waiting) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &count) ==
        MAELYS_SYS_ERR_WOULD_BLOCK);
    /* Removing an entry drops what was pending for it. */
    char file[PATH_ROOM];
    CHECK(in(removed, "f", file) && put(file, "") == 0);
    CHECK(maelys_sys_dirwatch_remove(dirwatch, again) == MAELYS_SYS_OK);
    CHECK(nothing_pending(dirwatch));
    /* Everything the entries held went back. */
    CHECK(count_open() == with_handle);
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    CHECK(count_open() == before);
    CHECK(unlink(file) == 0 && rmdir(removed) == 0 && rmdir(elsewhere) == 0);
    CHECK(rmdir(probe) == 0);
    return 0;
}

/* The entry follows the directory: an ancestor renamed says nothing. */
static int test_follows_directory(void) {
    char ancestor[PATH_ROOM], moved[PATH_ROOM], dir[PATH_ROOM], file[PATH_ROOM];
    CHECK(in(root, "ancestor", ancestor) && in(root, "ancestor-moved", moved));
    CHECK(mkdir(ancestor, 0700) == 0 && in(ancestor, "watched", dir) && mkdir(dir, 0700) == 0);
    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_dirwatch_entry_t entry = 0;
    CHECK(maelys_sys_dirwatch_create(2u, &dirwatch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 8, &entry) == MAELYS_SYS_OK);
    CHECK(rename(ancestor, moved) == 0);
    CHECK(quiet(dirwatch));
    CHECK(in(moved, "watched", dir) && in(dir, "f", file) && put(file, "") == 0);
    CHECK(changed_once(dirwatch, entry, 8));
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    CHECK(unlink(file) == 0 && rmdir(dir) == 0 && rmdir(moved) == 0);
    return 0;
}

/* One entry per directory, whatever the path; and what a path may not be. */
static int test_refusals(void) {
    char dir[PATH_ROOM], link_to_dir[PATH_ROOM], plain[PATH_ROOM], missing[PATH_ROOM];
    char under_missing[PATH_ROOM], link_to_root[PATH_ROOM], other_path[PATH_ROOM];
    char second[PATH_ROOM], third[PATH_ROOM], file[PATH_ROOM];
    CHECK(in(root, "refusals", dir) && mkdir(dir, 0700) == 0);
    CHECK(in(root, "refusals-link", link_to_dir) && symlink(dir, link_to_dir) == 0);
    CHECK(in(root, "refusals-plain", plain) && put(plain, "x") == 0);
    CHECK(in(root, "refusals-missing", missing) && in(missing, "below", under_missing));
    CHECK(in(root, "refusals-root-link", link_to_root) && symlink(root, link_to_root) == 0);
    CHECK(in(link_to_root, "refusals", other_path));
    CHECK(in(root, "refusals-second", second) && mkdir(second, 0700) == 0);
    CHECK(in(root, "refusals-third", third) && mkdir(third, 0700) == 0);
    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_dirwatch_entry_t entry = 0, other = 9;
    CHECK(maelys_sys_dirwatch_create(2u, &dirwatch) == MAELYS_SYS_OK);
    int with_handle = count_open();
    CHECK(maelys_sys_dirwatch_add(dirwatch, link_to_dir, 1, &other) == MAELYS_SYS_ERR_IDENTITY);
    CHECK(other == 0u);
    CHECK(maelys_sys_dirwatch_add(dirwatch, plain, 1, &other) == MAELYS_SYS_ERR_ARGUMENT);
    CHECK(maelys_sys_dirwatch_add(dirwatch, missing, 1, &other) == MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(maelys_sys_dirwatch_add(dirwatch, under_missing, 1, &other) ==
        MAELYS_SYS_ERR_NOT_FOUND);
    CHECK(count_open() == with_handle);

    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 1, &entry) == MAELYS_SYS_OK);
    int with_entry = count_open();
    /* The same directory, by the same path and through a linked ancestor. */
    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 2, &other) == MAELYS_SYS_ERR_EXISTS);
    CHECK(other == 0u);
    CHECK(maelys_sys_dirwatch_add(dirwatch, other_path, 2, &other) == MAELYS_SYS_ERR_EXISTS);
    CHECK(count_open() == with_entry);
    /* The refused adds took nothing from the first entry: a removal in the
     * directory is still reported to it. */
    CHECK(in(dir, "f", file) && put(file, "") == 0);
    CHECK(changed_once(dirwatch, entry, 1));
    CHECK(unlink(file) == 0);
    CHECK(changed_once(dirwatch, entry, 1));

    /* The caller's bound, with nothing left behind; a directory already
     * watched is still said to be so when the table is full. */
    CHECK(maelys_sys_dirwatch_add(dirwatch, second, 2, &other) == MAELYS_SYS_OK);
    int full = count_open();
    maelys_sys_dirwatch_entry_t none = 9;
    CHECK(maelys_sys_dirwatch_add(dirwatch, third, 3, &none) == MAELYS_SYS_ERR_CAPACITY);
    CHECK(none == 0u && count_open() == full);
    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 3, &none) == MAELYS_SYS_ERR_EXISTS);
    /* The refused directory was not left registered: it says nothing. */
    CHECK(in(third, "f", file) && put(file, "") == 0 && unlink(file) == 0);
    CHECK(nothing_pending(dirwatch));
    /* A released entry frees its place, and its number is not reused. */
    CHECK(maelys_sys_dirwatch_remove(dirwatch, other) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_add(dirwatch, third, 3, &none) == MAELYS_SYS_OK);
    CHECK(none > other && other > entry);
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    CHECK(unlink(link_to_dir) == 0 && unlink(link_to_root) == 0 && unlink(plain) == 0);
    CHECK(rmdir(dir) == 0 && rmdir(second) == 0 && rmdir(third) == 0);
    return 0;
}

#if defined(__APPLE__)
/* macOS holds one descriptor per entry: the process limit is ERR_OS with
 * EMFILE, and nothing is left behind. */
static int test_descriptor_limit(void) {
    char dir[PATH_ROOM];
    CHECK(in(root, "limit", dir) && mkdir(dir, 0700) == 0);
    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_dirwatch_entry_t entry = 9;
    CHECK(maelys_sys_dirwatch_create(2u, &dirwatch) == MAELYS_SYS_OK);
    struct rlimit saved, lowered;
    CHECK(getrlimit(RLIMIT_NOFILE, &saved) == 0);
    int highest = -1;
    for (int fd = 0; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) highest = fd;
    }
    int before = count_open();
    lowered = saved;
    lowered.rlim_cur = (rlim_t)(highest + 1);
    CHECK(setrlimit(RLIMIT_NOFILE, &lowered) == 0);
    errno = 0;
    maelys_sys_result_t result = maelys_sys_dirwatch_add(dirwatch, dir, 1, &entry);
    int error = errno;
    CHECK(setrlimit(RLIMIT_NOFILE, &saved) == 0);
    CHECK(result == MAELYS_SYS_ERR_OS && error == EMFILE && entry == 0u);
    CHECK(count_open() == before);
    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 1, &entry) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    CHECK(rmdir(dir) == 0);
    return 0;
}
#endif

/* The descriptor in a loop, on every backend of the host. */
static int test_in_a_loop(void) {
    static const maelys_sys_loop_backend_t backends[] = {
        MAELYS_SYS_LOOP_AUTO, MAELYS_SYS_LOOP_POLL
    };
    char dir[PATH_ROOM], file[PATH_ROOM];
    CHECK(in(root, "looped", dir) && mkdir(dir, 0700) == 0 && in(dir, "f", file));
    for (size_t round = 0; round < sizeof(backends) / sizeof(backends[0]); ++round) {
        maelys_sys_loop_t *loop = NULL;
        maelys_sys_dirwatch_t *dirwatch = NULL;
        maelys_sys_dirwatch_entry_t entry = 0;
        maelys_sys_watch_t watch = 0;
        CHECK(maelys_sys_loop_create(backends[round], &loop) == MAELYS_SYS_OK);
        CHECK(maelys_sys_dirwatch_create(4u, &dirwatch) == MAELYS_SYS_OK);
        CHECK(maelys_sys_loop_watch_fd(loop, maelys_sys_dirwatch_fd(dirwatch),
            MAELYS_SYS_INTEREST_READ, 77, &watch) == MAELYS_SYS_OK);
        CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 6, &entry) == MAELYS_SYS_OK);
        CHECK(put(file, "") == 0);
        maelys_sys_event_t events[4];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        uint64_t deadline = 0;
        CHECK(maelys_sys_deadline_after(3000, &deadline) == MAELYS_SYS_OK);
        CHECK(maelys_sys_loop_step(loop, deadline, events, 4u, &count, &step) == MAELYS_SYS_OK);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == 1u && events[0].token == 77);
        CHECK(events[0].flags & MAELYS_SYS_EVENT_READ);
        /* Polled to ERR_WOULD_BLOCK, the descriptor is quiet again. */
        maelys_sys_dirwatch_change_t change;
        size_t changes = 0;
        CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &changes) == MAELYS_SYS_OK);
        CHECK(changes == 1u && change.entry == entry && change.token == 6 &&
            change.flags == CHANGED);
        CHECK(maelys_sys_dirwatch_poll(dirwatch, &change, 1u, &changes) ==
            MAELYS_SYS_ERR_WOULD_BLOCK);
        CHECK(maelys_sys_deadline_after(100, &deadline) == MAELYS_SYS_OK);
        CHECK(maelys_sys_loop_step(loop, deadline, events, 4u, &count, &step) == MAELYS_SYS_OK);
        CHECK(step == MAELYS_SYS_STEP_TIMEOUT);
        CHECK(maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_OK);
        CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
        CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
        CHECK(unlink(file) == 0);
    }
    CHECK(rmdir(dir) == 0);
    return 0;
}

/* Destroying with live entries and changes pending leaves nothing open. */
static int test_destroy_with_pending(void) {
    char dir[PATH_ROOM], file[PATH_ROOM];
    CHECK(in(root, "pending", dir) && mkdir(dir, 0700) == 0 && in(dir, "f", file));
    int before = count_open();
    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_dirwatch_entry_t entry = 0;
    CHECK(maelys_sys_dirwatch_create(4u, &dirwatch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_add(dirwatch, dir, 1, &entry) == MAELYS_SYS_OK);
    CHECK(put(file, "") == 0);
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK && dirwatch == NULL);
    CHECK(count_open() == before);
    CHECK(maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK);
    CHECK(unlink(file) == 0 && rmdir(dir) == 0);
    return 0;
}

typedef struct stranger {
    maelys_sys_dirwatch_t *dirwatch;
    maelys_sys_dirwatch_entry_t entry;
    int refused;
} stranger_t;

static void *stranger_thread(void *context) {
    stranger_t *stranger = context;
    maelys_sys_dirwatch_entry_t entry = 9;
    maelys_sys_dirwatch_change_t change;
    size_t count = 9;
    maelys_sys_dirwatch_t *handle = stranger->dirwatch;
    stranger->refused =
        maelys_sys_dirwatch_add(handle, root, 1, &entry) == MAELYS_SYS_ERR_STATE &&
        entry == 0u &&
        maelys_sys_dirwatch_remove(handle, stranger->entry) == MAELYS_SYS_ERR_STATE &&
        maelys_sys_dirwatch_poll(handle, &change, 1u, &count) == MAELYS_SYS_ERR_STATE &&
        count == 0u &&
        maelys_sys_dirwatch_destroy(&handle) == MAELYS_SYS_ERR_STATE && handle != NULL;
    return NULL;
}

/* Like a loop, the handle belongs to the thread that made it. */
static int test_owner_thread(void) {
    stranger_t stranger = {NULL, 0, 0};
    maelys_sys_thread_t *thread = NULL;
    void *unused = NULL;
    CHECK(maelys_sys_dirwatch_create(2u, &stranger.dirwatch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_add(stranger.dirwatch, root, 1, &stranger.entry) ==
        MAELYS_SYS_OK);
    CHECK(maelys_sys_thread_create("stranger", stranger_thread, &stranger, &thread) ==
        MAELYS_SYS_OK);
    CHECK(maelys_sys_thread_join(&thread, &unused) == MAELYS_SYS_OK);
    CHECK(stranger.refused);
    /* The owner still holds a working handle. */
    CHECK(maelys_sys_dirwatch_remove(stranger.dirwatch, stranger.entry) == MAELYS_SYS_OK);
    CHECK(maelys_sys_dirwatch_destroy(&stranger.dirwatch) == MAELYS_SYS_OK);
    return 0;
}

int main(void) {
    const char *base = getenv("TMPDIR");
    if (!base || base[0] != '/') base = "/tmp";
    int written = snprintf(root, sizeof(root), "%s/maelys-sys-dirwatch.XXXXXX", base);
    if (written <= 0 || (size_t)written >= sizeof(root) || !mkdtemp(root)) return 1;
    int before = count_open();
    int failed = test_arguments() || test_entries_reported() || test_not_reported() ||
        test_coalescing_and_turns() || test_gone() || test_follows_directory() ||
        test_refusals() ||
#if defined(__APPLE__)
        test_descriptor_limit() ||
#endif
        test_in_a_loop() || test_destroy_with_pending() || test_owner_thread();
    if (failed) return 1;
    if (count_open() != before) {
        fprintf(stderr, "descriptors left open\n");
        return 1;
    }
    if (rmdir(root) != 0) {
        fprintf(stderr, "work directory not empty: %s\n", root);
        return 1;
    }
    puts("ok - dirwatch arguments");
    puts("ok - dirwatch reports creation, removal, rename and replacement by rename");
    puts("ok - dirwatch reports no write in place, metadata or subdirectory content");
    puts("ok - dirwatch coalesces and serves entries in turn");
    puts("ok - dirwatch GONE releases at delivery; numbers never reused");
    puts("ok - dirwatch follows the directory, not its path");
    puts("ok - dirwatch one entry per directory; links, files and missing paths refused");
    puts("ok - dirwatch in a loop on every backend; destroy leaves nothing open");
    puts("ok - dirwatch owner thread only");
    return 0;
}
