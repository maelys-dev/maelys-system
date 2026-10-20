#define _POSIX_C_SOURCE 200809L

#include "maelys/sys.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * A program started as the leader of an owned group, followed, and
 * stopped on a timer.
 *
 * owned-launch KEEPER GRACE_MS PATH [ARG...]: PATH is started through the
 * keeper at KEEPER with /dev/null on 0 and one pipe on 1 and 2. Its
 * output is relayed line by line. Its end, and every change of the
 * group, is read on the two descriptors the handle lends. After GRACE_MS
 * the group is asked to stop: SIGTERM, half the grace, SIGKILL. Whatever
 * the program started in its group ends with it; and had this process
 * been killed instead, the keeper would have ended the group all the
 * same, which is the point of the component.
 */

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

static int usage(void) {
    fprintf(stderr, "usage: owned-launch KEEPER GRACE_MS PATH [ARG...]\n");
    return 2;
}

/* Reads what the pipe holds, without waiting; 1 at its end. */
static int drain_output(int *fd) {
    if (*fd < 0) return 1;
    for (;;) {
        struct pollfd ready = {*fd, POLLIN, 0};
        if (poll(&ready, 1, 0) <= 0) return 0;
        char chunk[256];
        ssize_t got = read(*fd, chunk, sizeof(chunk));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            (void)close(*fd);
            *fd = -1;
            return 1;
        }
        relay(chunk, (size_t)got);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("usage: owned-launch KEEPER GRACE_MS PATH [ARG...]");
        return 0;
    }
    if (argc < 4) return usage();
    char *end = NULL;
    errno = 0;
    unsigned long grace = strtoul(argv[2], &end, 10);
    if (errno || !end || *end || grace == 0) return usage();

    int out[2];
    if (pipe(out) != 0) return 1;
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull < 0) return 1;
    maelys_sys_process_fd_t table[] = {{devnull, 0}, {out[1], 1}, {out[1], 2}};
    maelys_sys_process_options_t program = {.path = argv[3], .argv = argv + 3,
        .fds = table, .fd_count = 3};
    maelys_sys_owned_options_t options = {.keeper_path = argv[1], .program = &program,
        .flags = MAELYS_SYS_OWNED_END_WITH_LEADER, .heartbeat_ms = 500, .observe_ms = 2000,
        .linger_ms = 2000};
    if (maelys_sys_deadline_after(30000, &options.start_deadline_ms) != MAELYS_SYS_OK) return 1;
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_result_t started = maelys_sys_owned_spawn(&options, &owned);
    (void)close(out[1]);
    (void)close(devnull);
    if (started != MAELYS_SYS_OK) {
        fprintf(stderr, "start failed: result %d (%s)\n", (int)started, strerror(errno));
        return 1;
    }

    uint64_t deadline = 0;
    if (maelys_sys_deadline_after(grace, &deadline) != MAELYS_SYS_OK) return 1;
    int stopped = 0;
    maelys_sys_owned_state_t state;
    for (;;) {
        if (maelys_sys_owned_poll(owned, &state) != MAELYS_SYS_OK) return 1;
        if (state.leader_ended || state.keeper_reaped) break;
        uint64_t now = 0;
        (void)maelys_sys_monotonic_ms(&now);
        if (!stopped && now >= deadline) {
            uint32_t seq = 0;
            if (maelys_sys_owned_stop(owned, grace / 2, &seq) != MAELYS_SYS_OK) return 1;
            stopped = 1;
        }
        struct pollfd fds[3] = {{out[0], POLLIN, 0}, {maelys_sys_owned_fd(owned), POLLIN, 0},
            {maelys_sys_owned_keeper_fd(owned), POLLIN, 0}};
        int timeout = stopped ? 1000 : (deadline > now ? (int)(deadline - now) : 0);
        if (poll(fds, 3, timeout) < 0 && errno != EINTR) return 1;
        (void)drain_output(&out[0]);
    }
    /* The program is over: what it wrote, then how it ended, then the
     * group as a whole. A program stopped by the ladder may leave the
     * other end of the pipe in a child: its output is not awaited. */
    if (!stopped) {
        while (!drain_output(&out[0])) {
            struct pollfd ready = {out[0], POLLIN, 0};
            if (poll(&ready, 1, 2000) <= 0) break;
        }
        if (carried) relay("\n", 1);
    }
    uint64_t final_deadline = 0;
    if (maelys_sys_deadline_after(10000, &final_deadline) != MAELYS_SYS_OK) return 1;
    if (maelys_sys_owned_wait(owned, final_deadline, &state) != MAELYS_SYS_OK) return 1;
    if (state.leader.exited) {
        printf("%s: exit %d\n", stopped ? "stopped" : "ended", state.leader.exit_code);
    } else if (state.leader.signaled) {
        printf("%s: signal %d\n", stopped ? "stopped" : "ended", state.leader.term_signal);
    }
    printf("group: %s\n", state.phase == MAELYS_SYS_OWNED_ENDED ? "ended" : "uncertain");
    if (out[0] >= 0) (void)close(out[0]);
    return maelys_sys_owned_release(&owned) == MAELYS_SYS_OK ? 0 : 1;
}
