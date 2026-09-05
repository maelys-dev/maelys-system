#include "maelys/sys.h"

#include <errno.h>
#include <netinet/in.h>
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

/*
 * A descriptor closed before unwatch, a contract fault: the registration is
 * released all the same, and the number can be watched again once reused.
 */
static int closed_before_unwatch(maelys_sys_loop_backend_t backend) {
    fixture_t fixture;
    CHECK(fixture_open(&fixture, backend, MAELYS_SYS_INTEREST_READ, 1) == 0);
    int number = fixture.sockets[0];
    CHECK(maelys_sys_fd_close(&fixture.sockets[0]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&fixture.sockets[1]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_unwatch(fixture.loop, fixture.watch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_unwatch(fixture.loop, fixture.watch) == MAELYS_SYS_ERR_NOT_FOUND);
    int again[2] = {-1, -1};
    CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, again) == MAELYS_SYS_OK);
    CHECK(again[0] == number || again[1] == number);
    int reused = again[0] == number ? again[0] : again[1];
    maelys_sys_watch_t watch = 0;
    CHECK(maelys_sys_loop_watch_fd(fixture.loop, reused, MAELYS_SYS_INTEREST_READ,
        2, &watch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_unwatch(fixture.loop, watch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&again[0]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_close(&again[1]) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_destroy(&fixture.loop) == MAELYS_SYS_OK);
    return 0;
}

/* Four ready watches, a caller array of two: every token within two steps. */
static int fairness(maelys_sys_loop_backend_t backend) {
    enum { PAIRS = 4 };
    maelys_sys_loop_t *loop = NULL;
    int pairs[PAIRS][2];
    maelys_sys_watch_t watches[PAIRS];
    unsigned seen = 0;
    CHECK(maelys_sys_loop_create(backend, &loop) == MAELYS_SYS_OK);
    for (size_t i = 0; i < PAIRS; ++i) {
        CHECK(maelys_sys_socketpair_cloexec(SOCK_STREAM, pairs[i]) == MAELYS_SYS_OK);
        CHECK(maelys_sys_loop_watch_fd(loop, pairs[i][0], MAELYS_SYS_INTEREST_READ,
            (maelys_sys_token_t)(i + 1u), &watches[i]) == MAELYS_SYS_OK);
        CHECK(write(pairs[i][1], "x", 1) == 1);
    }
    for (int round = 0; round < 2; ++round) {
        maelys_sys_event_t events[2];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
        CHECK(step_within(loop, 500, events, 2, &count, &step) == 0);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == 2);
        for (size_t i = 0; i < count; ++i) seen |= 1u << (events[i].token - 1u);
    }
    /* Nothing was read: the first two stayed ready, the others still came. */
    CHECK(seen == 0xfu);
    for (size_t i = 0; i < PAIRS; ++i) {
        CHECK(maelys_sys_loop_unwatch(loop, watches[i]) == MAELYS_SYS_OK);
        CHECK(maelys_sys_fd_close(&pairs[i][0]) == MAELYS_SYS_OK);
        CHECK(maelys_sys_fd_close(&pairs[i][1]) == MAELYS_SYS_OK);
    }
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

/*
 * What loop.h promises about HUP and ERROR, and what it leaves to the host:
 * a WRITE-only watch sees the peer's half-close on Linux and not on macOS;
 * a reset is READ|HUP with ERROR everywhere but on the poll backend of macOS.
 */
static int hup_and_error_by_host(maelys_sys_loop_backend_t backend) {
    fixture_t fixture;
    maelys_sys_event_t events[4];
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_TIMEOUT;
    CHECK(fixture_open(&fixture, backend, MAELYS_SYS_INTEREST_WRITE, 1) == 0);
    CHECK(shutdown(fixture.sockets[1], SHUT_WR) == 0);
    CHECK(step_within(fixture.loop, 500, events, 4, &count, &step) == 0);
    CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == 1);
    CHECK(events[0].flags & MAELYS_SYS_EVENT_WRITE);
#if defined(__linux__)
    CHECK(events[0].flags & MAELYS_SYS_EVENT_HUP);
#else
    CHECK(!(events[0].flags & MAELYS_SYS_EVENT_HUP));
#endif
    CHECK(fixture_close(&fixture) == 0);

    /* A TCP reset: the peer closes with a zero linger. */
    maelys_sys_socket_t *listener = NULL;
    maelys_sys_socket_t *client = NULL;
    maelys_sys_socket_t *accepted = NULL;
    struct sockaddr_in address;
    socklen_t length = (socklen_t)sizeof(address);
    maelys_sys_connect_state_t state;
    maelys_sys_loop_t *loop = NULL;
    maelys_sys_watch_t watch = 0;
    CHECK(maelys_sys_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener) ==
        MAELYS_SYS_OK);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(maelys_sys_socket_bind(listener, (const struct sockaddr *)&address,
        length) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_listen(listener, 1) == MAELYS_SYS_OK);
    CHECK(getsockname(maelys_sys_socket_native_fd(listener),
        (struct sockaddr *)&address, &length) == 0);
    CHECK(maelys_sys_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &client) ==
        MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_connect_start(client, (const struct sockaddr *)&address,
        length, &state) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_create(backend, &loop) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_watch_fd(loop, maelys_sys_socket_native_fd(listener),
        MAELYS_SYS_INTEREST_READ, 1, &watch) == MAELYS_SYS_OK);
    CHECK(step_within(loop, 500, events, 4, &count, &step) == 0);
    CHECK(count == 1);
    CHECK(maelys_sys_socket_accept(listener, NULL, NULL, &accepted) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_OK);
    if (state == MAELYS_SYS_CONNECT_IN_PROGRESS) {
        CHECK(maelys_sys_loop_watch_fd(loop, maelys_sys_socket_native_fd(client),
            MAELYS_SYS_INTEREST_WRITE, 2, &watch) == MAELYS_SYS_OK);
        CHECK(step_within(loop, 500, events, 4, &count, &step) == 0);
        CHECK(maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_OK);
    }
    CHECK(maelys_sys_socket_connect_complete(client) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_watch_fd(loop, maelys_sys_socket_native_fd(accepted),
        MAELYS_SYS_INTEREST_READ, 3, &watch) == MAELYS_SYS_OK);
    struct linger abort = {.l_onoff = 1, .l_linger = 0};
    CHECK(setsockopt(maelys_sys_socket_native_fd(client), SOL_SOCKET, SO_LINGER,
        &abort, sizeof(abort)) == 0);
    CHECK(maelys_sys_socket_release(&client) == MAELYS_SYS_OK);
    CHECK(step_within(loop, 500, events, 4, &count, &step) == 0);
    CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == 1 && events[0].token == 3);
    CHECK(events[0].flags & MAELYS_SYS_EVENT_READ);
    CHECK(events[0].flags & MAELYS_SYS_EVENT_HUP);
#if defined(__linux__)
    CHECK(events[0].flags & MAELYS_SYS_EVENT_ERROR);
#else
    if (backend == MAELYS_SYS_LOOP_POLL) {
        CHECK(!(events[0].flags & MAELYS_SYS_EVENT_ERROR));
    } else {
        CHECK(events[0].flags & MAELYS_SYS_EVENT_ERROR);
    }
#endif
    CHECK(maelys_sys_loop_unwatch(loop, watch) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_release(&accepted) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_release(&listener) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK);
    return 0;
}

static int run_backend(maelys_sys_loop_backend_t backend, const char *label) {
    CHECK(maelys_sys_loop_backend_available(backend));
    CHECK(peer_half_close(backend) == 0);
    CHECK(merged_directions(backend) == 0);
    CHECK(wake_with_full_array(backend) == 0);
    CHECK(closed_before_unwatch(backend) == 0);
    CHECK(fairness(backend) == 0);
    CHECK(hup_and_error_by_host(backend) == 0);
    printf("ok - %s backend parity\n", label);
    return 0;
}

int main(void) {
    if (run_backend(MAELYS_SYS_LOOP_POLL, "poll") != 0) return 1;
    if (run_backend(MAELYS_SYS_LOOP_AUTO, "native") != 0) return 1;
    return 0;
}
