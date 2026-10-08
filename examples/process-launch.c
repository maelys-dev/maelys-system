#define _POSIX_C_SOURCE 200809L

#include "maelys/sys.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * A program started with exactly three descriptors, followed in a loop,
 * and stopped on a timer.
 *
 * process-launch GRACE_MS PATH [ARG...]: PATH is started with /dev/null on
 * 0 and one pipe on 1 and 2, the layout maelys/sys/process.h calls STDIO.
 * Its output is relayed line by line. Its end is a loop event, like the
 * pipe's readiness: nothing polls. After GRACE_MS a timer fires and the
 * program is stopped with the ladder: SIGTERM, half the grace, SIGKILL.
 * A program that ended by itself has its output drained to the end; one
 * stopped by the ladder has its pipe dropped, since whatever it started
 * may still hold the other end.
 */

/* Whole lines out of the chunks a pipe delivers. */
static char carry[512];
static size_t carried;

static void relay(const char *chunk, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        if (chunk[index] == '\n' || carried == sizeof(carry) - 1) {
            carry[carried] = '\0';
            printf("out: %s\n", carry);
            carried = 0;
            if (chunk[index] == '\n') continue;
        }
        carry[carried++] = chunk[index];
    }
}

static void relay_rest(void) {
    if (carried) relay("\n", 1);
}

enum { OUTPUT = 1, ENDED = 2, DEADLINE = 3 };

static int usage(void) {
    fprintf(stderr, "usage: process-launch GRACE_MS PATH [ARG...]\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("usage: process-launch GRACE_MS PATH [ARG...]");
        return 0;
    }
    char *end = NULL;
    errno = 0;
    unsigned long grace = argc >= 3 ? strtoul(argv[1], &end, 10) : 0;
    if (argc < 3 || errno || !end || *end || grace == 0 || argv[2][0] != '/') return usage();

    /* The pipe the program writes to, and nothing to read from. */
    int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int output[2];
    if (devnull < 0 || maelys_sys_pipe_cloexec(output) != MAELYS_SYS_OK) {
        perror("open");
        return 1;
    }
    /* Exactly these three in the program: 0, 1 and 2, and not this
     * example's own descriptors, nor the read end of the pipe. */
    maelys_sys_process_fd_t table[3] = {{devnull, 0}, {output[1], 1}, {output[1], 2}};
    maelys_sys_process_options_t options = {
        .path = argv[2], .argv = argv + 2, .fds = table, .fd_count = 3
    };
    maelys_sys_process_t *process = NULL;
    maelys_sys_result_t result = maelys_sys_process_spawn(&options, &process);
    if (result != MAELYS_SYS_OK) {
        /* NOT_FOUND names a missing path; anything else is errno. */
        fprintf(stderr, "%s: %s\n", argv[2], result == MAELYS_SYS_ERR_OS ?
            strerror(errno) : maelys_sys_result_string(result));
        return 1;
    }
    /* The write end belongs to the program now: closed here, the pipe ends
     * when the program does. */
    if (maelys_sys_fd_close(&output[1]) != MAELYS_SYS_OK ||
        maelys_sys_fd_set_nonblocking(output[0]) != MAELYS_SYS_OK) {
        return 1;
    }

    maelys_sys_loop_t *loop = NULL;
    maelys_sys_watch_t output_watch = 0, end_watch = 0;
    maelys_sys_timer_t timer = 0;
    uint64_t deadline = 0;
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) != MAELYS_SYS_OK ||
        maelys_sys_loop_watch_fd(loop, output[0], MAELYS_SYS_INTEREST_READ, OUTPUT,
            &output_watch) != MAELYS_SYS_OK ||
        maelys_sys_loop_watch_fd(loop, maelys_sys_process_exit_fd(process),
            MAELYS_SYS_INTEREST_READ, ENDED, &end_watch) != MAELYS_SYS_OK ||
        maelys_sys_deadline_after(grace, &deadline) != MAELYS_SYS_OK ||
        maelys_sys_loop_timer_add(loop, deadline, DEADLINE, &timer) != MAELYS_SYS_OK) {
        return 1;
    }

    maelys_sys_process_status_t status;
    int ended = 0, output_open = 1;
    while (!ended || output_open) {
        maelys_sys_event_t events[4];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        if (maelys_sys_loop_step(loop, MAELYS_SYS_DEADLINE_INFINITE, events, 4,
                &count, &step) != MAELYS_SYS_OK) {
            return 1;
        }
        for (size_t index = 0; index < count; ++index) {
            if (events[index].token == OUTPUT) {
                /* Bounded work per event: one read, then back to the loop. */
                char chunk[256];
                ssize_t got = read(output[0], chunk, sizeof(chunk));
                if (got > 0) {
                    relay(chunk, (size_t)got);
                } else if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
                    relay_rest();
                    if (maelys_sys_loop_unwatch(loop, output_watch) != MAELYS_SYS_OK) return 1;
                    output_open = 0;
                }
            } else if (events[index].token == ENDED && !ended) {
                /* Readable: the program has ended, and a wait with a
                 * deadline already past reaps it without waiting. */
                uint64_t now = 0;
                if (maelys_sys_monotonic_ms(&now) != MAELYS_SYS_OK ||
                    maelys_sys_process_wait(process, now, &status) != MAELYS_SYS_OK) {
                    return 1;
                }
                ended = 1;
                if (maelys_sys_loop_unwatch(loop, end_watch) != MAELYS_SYS_OK) return 1;
                if (status.exited) printf("ended: exit %d\n", status.exit_code);
                else printf("ended: signal %d\n", status.term_signal);
            } else if (events[index].token == DEADLINE && !ended) {
                /* The ladder is bounded by its two durations; it reaps on
                 * its own, so the exit descriptor is unwatched first. */
                if (maelys_sys_loop_unwatch(loop, end_watch) != MAELYS_SYS_OK) return 1;
                result = maelys_sys_process_terminate(process, grace / 2, grace / 2, &status);
                ended = 1;
                if (result == MAELYS_SYS_OK) printf("stopped: signal %d\n", status.term_signal);
                else printf("stopped: %s\n", maelys_sys_result_string(result));
                if (output_open) {
                    relay_rest();
                    if (maelys_sys_loop_unwatch(loop, output_watch) != MAELYS_SYS_OK) return 1;
                    output_open = 0;
                }
            }
        }
        fflush(stdout);
    }
    int ok = maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK;
    ok = maelys_sys_process_release(&process) == MAELYS_SYS_OK && ok;
    ok = maelys_sys_fd_close(&output[0]) == MAELYS_SYS_OK && ok;
    ok = maelys_sys_fd_close(&devnull) == MAELYS_SYS_OK && ok;
    return ok ? 0 : 1;
}
