#define _POSIX_C_SOURCE 200809L

#include "maelys/sys.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * "The entries of this directory changed: reread it", in a loop.
 *
 * The three rules a consumer of maelys/sys/dirwatch.h has to hold:
 *
 * 1. Add, then read. What happens between the add and the first read is
 *    reported like anything later; reading first would lose it.
 * 2. One poll each time the descriptor is readable, with an array of
 *    entry_capacity elements. An entry is reported at most once per call,
 *    so that array always fits and nothing stays pending in the handle;
 *    whatever the kernel took in meanwhile keeps the descriptor readable
 *    for the next turn. Polling again "until nothing is left" would keep
 *    this thread here for as long as someone writes.
 * 3. A change says "reread" and nothing more: no name, no kind.
 */

/* What a consumer does with "reread": here, count the entries. */
static long count_entries(const char *path) {
    DIR *directory = opendir(path);
    if (!directory) return -1;
    long count = 0;
    for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) ++count;
    }
    return closedir(directory) == 0 ? count : -1;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("usage: directory-watch CHANGES DIRECTORY...");
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long wanted = argc >= 3 ? strtoul(argv[1], &end, 10) : 0;
    if (argc < 3 || errno || !end || *end || wanted == 0) {
        fprintf(stderr, "usage: directory-watch CHANGES DIRECTORY...\n");
        return 2;
    }
    char **paths = argv + 2;
    size_t capacity = (size_t)argc - 2u;

    maelys_sys_dirwatch_t *dirwatch = NULL;
    maelys_sys_result_t result = maelys_sys_dirwatch_create(capacity, &dirwatch);
    if (result != MAELYS_SYS_OK) {
        /* ERR_UNSUPPORTED on a host without the facility: a consumer falls
         * back to rereading on a timer. */
        fprintf(stderr, "dirwatch_create: %s\n", maelys_sys_result_string(result));
        return 1;
    }
    /* The array of rule 2, sized once. */
    maelys_sys_dirwatch_change_t *changes = calloc(capacity, sizeof(*changes));
    if (!changes) return 1;

    size_t live = 0;
    for (size_t index = 0; index < capacity; ++index) {
        maelys_sys_dirwatch_entry_t entry = 0;
        /* The token is what comes back with a change: here, which path. */
        result = maelys_sys_dirwatch_add(dirwatch, paths[index], index, &entry);
        if (result != MAELYS_SYS_OK) {
            fprintf(stderr, "%s: %s\n", paths[index], maelys_sys_result_string(result));
            return 1;
        }
        ++live;
        /* Rule 1: the first read comes after the add. */
        printf("watching %s: %ld entries\n", paths[index], count_entries(paths[index]));
    }
    fflush(stdout);

    maelys_sys_loop_t *loop = NULL;
    maelys_sys_watch_t watch = 0;
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) != MAELYS_SYS_OK ||
        maelys_sys_loop_watch_fd(loop, maelys_sys_dirwatch_fd(dirwatch),
            MAELYS_SYS_INTEREST_READ, 1, &watch) != MAELYS_SYS_OK) {
        return 1;
    }

    unsigned long seen = 0;
    while (seen < wanted && live > 0) {
        maelys_sys_event_t events[4];
        size_t ready = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        if (maelys_sys_loop_step(loop, MAELYS_SYS_DEADLINE_INFINITE, events, 4,
                &ready, &step) != MAELYS_SYS_OK) {
            return 1;
        }
        for (size_t index = 0; index < ready; ++index) {
            if (events[index].token != 1) continue;
            /* Rule 2: one call, then back to the loop. ERR_WOULD_BLOCK is
             * not an error: the descriptor may be readable with nothing to
             * report. */
            size_t count = 0;
            result = maelys_sys_dirwatch_poll(dirwatch, changes, capacity, &count);
            if (result != MAELYS_SYS_OK && result != MAELYS_SYS_ERR_WOULD_BLOCK) return 1;
            for (size_t position = 0; position < count; ++position) {
                const char *path = paths[changes[position].token];
                if (changes[position].flags & MAELYS_SYS_DIRWATCH_GONE) {
                    /* The entry is already released: nothing to remove. */
                    printf("gone %s\n", path);
                    --live;
                } else {
                    /* Rule 3. CHANGED, alone or with OVERFLOW, asks the same. */
                    printf("reread %s: %ld entries\n", path, count_entries(path));
                }
                ++seen;
            }
            fflush(stdout);
        }
    }

    /* Unwatch before destroy: the loop borrows the descriptor. */
    int ok = maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_OK;
    ok = maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK && ok;
    ok = maelys_sys_dirwatch_destroy(&dirwatch) == MAELYS_SYS_OK && ok;
    free(changes);
    return ok ? 0 : 1;
}
