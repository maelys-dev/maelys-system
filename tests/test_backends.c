#include "maelys/sys.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Backend parity: the same scenario must produce the same event array on the
 * reference poll backend and on the native backend of the host.
 */

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s\n", \
            __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct fixture {
    maelys_sys_loop_t *loop;
    int sockets[2];
    maelys_sys_watch_t watch;
} fixture_t;

static int fixture_open(
    fixture_t *fixture, maelys_sys_loop_backend_t backend, unsigned interests,
    maelys_sys_token_t token) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->sockets[0] = -1;
    fixture->sockets[1] = -1;
    CHECK(maelys_sys_loop_create(backend, &fixture->loop) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, fixture->sockets) ==
        MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_watch_fd(fixture->loop, fixture->sockets[0],
        interests, token, &fixture->watch) == MAELYS_SYS_OK);
    return 0;
}

static int fixture_close(fixture_t *fixture) {
    CHECK(maelys_sys_loop_unwatch(fixture->loop, fixture->watch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&fixture->sockets[0]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&fixture->sockets[1]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_destroy(&fixture->loop) == MAELYS_SYS_OK);
    return 0;
}

static int step_within(
    maelys_sys_loop_t *loop, uint64_t timeout_ms, maelys_sys_event_t *events,
    size_t capacity, size_t *out_count, maelys_sys_step_result_t *out_step) {
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(timeout_ms, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_step(
        loop, deadline, events, capacity, out_count, out_step) == MAELYS_SYS_OK);
    return 0;
}

/* Peer shutdown(SHUT_WR): READ|HUP, exactly, on every backend. */
static int peer_half_close(maelys_sys_loop_backend_t backend) {
    fixture_t fixture;
    CHECK(fixture_open(&fixture, backend, MAELYS_SYS_INTEREST_READ, 1) == 0);
    CHECK(shutdown(fixture.sockets[1], SHUT_WR) == 0);
    maelys_sys_event_t events[4];
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
    CHECK(step_within(fixture.loop, 500, events, 4, &count, &step) == 0);
    CHECK(step == MAELYS_SYS_STEP_PROGRESS);
    CHECK(count == 1);
    CHECK(events[0].token == 1);
    CHECK(events[0].flags == (MAELYS_SYS_EVENT_READ | MAELYS_SYS_EVENT_HUP));
    char byte = 0;
    CHECK(read(fixture.sockets[0], &byte, 1) == 0);
    CHECK(fixture_close(&fixture) == 0);
    return 0;
}

/* READ|WRITE interest, both ready: one event carrying both flags. */
static int merged_directions(maelys_sys_loop_backend_t backend) {
    fixture_t fixture;
    CHECK(fixture_open(&fixture, backend,
        MAELYS_SYS_INTEREST_READ | MAELYS_SYS_INTEREST_WRITE, 7) == 0);
    CHECK(write(fixture.sockets[1], "x", 1) == 1);
    maelys_sys_event_t events[4];
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
    CHECK(step_within(fixture.loop, 500, events, 4, &count, &step) == 0);
    CHECK(step == MAELYS_SYS_STEP_PROGRESS);
    CHECK(count == 1);
    CHECK(events[0].token == 7);
    CHECK(events[0].flags == (MAELYS_SYS_EVENT_READ | MAELYS_SYS_EVENT_WRITE));
    CHECK(fixture_close(&fixture) == 0);
    return 0;
}

/*
 * One ready descriptor plus one wake, caller capacity 1: both must surface,
 * each exactly once, whatever order the backend reports them in.
 */
static int wake_with_full_array(maelys_sys_loop_backend_t backend) {
    fixture_t fixture;
    CHECK(fixture_open(&fixture, backend, MAELYS_SYS_INTEREST_READ, 1) == 0);
    CHECK(write(fixture.sockets[1], "x", 1) == 1);
    CHECK(maelys_sys_loop_wake(fixture.loop) == MAELYS_SYS_OK);
    int wakes = 0;
    int reads = 0;
    for (int round = 0; round < 2; ++round) {
        maelys_sys_event_t event;
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        CHECK(step_within(fixture.loop, 500, &event, 1, &count, &step) == 0);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS);
        CHECK(count == 1);
        if (event.flags & MAELYS_SYS_EVENT_WAKE) {
            CHECK(event.token == 0);
            ++wakes;
        } else {
            CHECK(event.token == 1);
            CHECK(event.flags & MAELYS_SYS_EVENT_READ);
            char byte = 0;
            CHECK(read(fixture.sockets[0], &byte, 1) == 1);
            ++reads;
        }
    }
    CHECK(wakes == 1);
    CHECK(reads == 1);
    /* Nothing lingers: neither a duplicate wake nor a stale readiness. */
    maelys_sys_event_t event;
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_PROGRESS;
    CHECK(step_within(fixture.loop, 20, &event, 1, &count, &step) == 0);
    CHECK(step == MAELYS_SYS_STEP_TIMEOUT);
    CHECK(count == 0);
    CHECK(fixture_close(&fixture) == 0);
    return 0;
}

static int run_backend(maelys_sys_loop_backend_t backend, const char *label) {
    CHECK(maelys_sys_loop_backend_available(backend));
    CHECK(peer_half_close(backend) == 0);
    CHECK(merged_directions(backend) == 0);
    CHECK(wake_with_full_array(backend) == 0);
    printf("ok - %s backend parity\n", label);
    return 0;
}

int main(void) {
    if (run_backend(MAELYS_SYS_LOOP_POLL, "poll") != 0) return 1;
    if (run_backend(MAELYS_SYS_LOOP_AUTO, "native") != 0) return 1;
    return 0;
}
