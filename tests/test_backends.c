#include "maelys/sys.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
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
static int timer_due_now(maelys_sys_loop_t *loop, maelys_sys_token_t token) {
    uint64_t now = 0;
    maelys_sys_timer_t timer = 0;
    CHECK(maelys_sys_monotonic_ms(&now) == MAELYS_SYS_OK);
    CHECK(maelys_sys_loop_timer_add(loop, now, token, &timer) == MAELYS_SYS_OK);
    return 0;
}

/*
 * A timer that is due again at every step, as a periodic timer is for a
 * consumer slower than its period, does not keep a readable descriptor
 * unheard: each step reports both.
 */
static int timer_always_due_and_descriptor(maelys_sys_loop_backend_t backend) {
    fixture_t fixture;
    CHECK(fixture_open(&fixture, backend, MAELYS_SYS_INTEREST_READ, 1) == 0);
    CHECK(write(fixture.sockets[1], "x", 1) == 1);
    CHECK(timer_due_now(fixture.loop, 2) == 0);
    for (int turn = 0; turn < 8; ++turn) {
        maelys_sys_event_t events[4];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_STOPPED;
        CHECK(step_within(fixture.loop, 500, events, 4, &count, &step) == 0);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == 2);
        int descriptor = 0, timer = 0;
        for (size_t index = 0; index < count; ++index) {
            if (events[index].token == 1 && events[index].flags == MAELYS_SYS_EVENT_READ) {
                ++descriptor;
            }
            if (events[index].token == 2 && events[index].flags == MAELYS_SYS_EVENT_TIMER) {
                ++timer;
            }
        }
        CHECK(descriptor == 1 && timer == 1);
        CHECK(timer_due_now(fixture.loop, 2) == 0);
    }
    CHECK(fixture_close(&fixture) == 0);
    return 0;
}

/*
 * An array of one, which a timer always due and a descriptor always
 * readable could each fill for ever: they go first in turn, so neither
 * waits more than one step for the other.
 */
static int timer_and_descriptor_take_turns(maelys_sys_loop_backend_t backend) {
    fixture_t fixture;
    CHECK(fixture_open(&fixture, backend, MAELYS_SYS_INTEREST_READ, 1) == 0);
    CHECK(write(fixture.sockets[1], "x", 1) == 1);
    CHECK(timer_due_now(fixture.loop, 2) == 0);
    int descriptors = 0, timers = 0;
    for (int turn = 0; turn < 8; ++turn) {
        maelys_sys_event_t event;
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_STOPPED;
        CHECK(step_within(fixture.loop, 500, &event, 1, &count, &step) == 0);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == 1);
        if (event.flags == MAELYS_SYS_EVENT_TIMER) {
            CHECK(event.token == 2);
            ++timers;
            CHECK(timer_due_now(fixture.loop, 2) == 0);
        } else {
            CHECK(event.token == 1 && event.flags == MAELYS_SYS_EVENT_READ);
            ++descriptors;
        }
    }
    CHECK(descriptors == 4 && timers == 4);
    CHECK(fixture_close(&fixture) == 0);
    return 0;
}

/*
 * More timers due than the array holds, beside a readable descriptor:
 * every timer fires exactly once, none is spent by a step that does not
 * return it, and the descriptor is heard on the steps where it goes first.
 */
static int more_timers_due_than_room(maelys_sys_loop_backend_t backend) {
    enum { TIMERS = 9 };
    fixture_t fixture;
    CHECK(fixture_open(&fixture, backend, MAELYS_SYS_INTEREST_READ, 1) == 0);
    CHECK(write(fixture.sockets[1], "x", 1) == 1);
    for (unsigned index = 0; index < TIMERS; ++index) {
        CHECK(timer_due_now(fixture.loop, 100u + index) == 0);
    }
    static const size_t expected_count[4] = {4, 4, 3, 1};
    static const int expected_descriptor[4] = {1, 0, 1, 1};
    unsigned fired = 0;
    for (int turn = 0; turn < 4; ++turn) {
        maelys_sys_event_t events[4];
        size_t count = 0;
        maelys_sys_step_result_t step = MAELYS_SYS_STEP_STOPPED;
        CHECK(step_within(fixture.loop, 500, events, 4, &count, &step) == 0);
        CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == expected_count[turn]);
        int descriptor = 0;
        for (size_t index = 0; index < count; ++index) {
            if (events[index].flags == MAELYS_SYS_EVENT_TIMER) {
                maelys_sys_token_t token = events[index].token;
                CHECK(token >= 100u && token < 100u + TIMERS);
                CHECK(!(fired & (1u << (token - 100u))));
                fired |= 1u << (token - 100u);
            } else {
                CHECK(events[index].token == 1);
                ++descriptor;
            }
        }
        CHECK(descriptor == expected_descriptor[turn]);
    }
    CHECK(fired == (1u << TIMERS) - 1u);
    CHECK(fixture_close(&fixture) == 0);
    return 0;
}

/*
 * A loop does not cross fork, and the hosts differ in how it fails: the
 * child shares the parent's epoll instance on Linux, so its unwatch takes
 * the registration from the parent, which then hears nothing of a readable
 * descriptor; the kqueue is not inherited, and poll has no kernel object,
 * so there the parent is untouched.
 */
static int forked_child_unwatches(maelys_sys_loop_backend_t backend) {
    fixture_t fixture;
    CHECK(fixture_open(&fixture, backend, MAELYS_SYS_INTEREST_READ, 9) == 0);
    int shared = strcmp(maelys_sys_loop_backend_name(fixture.loop), "epoll") == 0;
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        maelys_sys_result_t result = maelys_sys_loop_unwatch(fixture.loop, fixture.watch);
        _exit(result == MAELYS_SYS_OK ? 0 : 1);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(write(fixture.sockets[1], "x", 1) == 1);
    maelys_sys_event_t events[4];
    size_t count = 0;
    maelys_sys_step_result_t step = MAELYS_SYS_STEP_STOPPED;
    CHECK(step_within(fixture.loop, 200, events, 4, &count, &step) == 0);
    if (shared) {
        CHECK(step == MAELYS_SYS_STEP_TIMEOUT && count == 0);
    } else {
        CHECK(step == MAELYS_SYS_STEP_PROGRESS && count == 1 && events[0].token == 9);
    }
    CHECK(fixture_close(&fixture) == 0);
    return 0;
}

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

/* A non-blocking receive with nothing to read is ERR_WOULD_BLOCK. */
static int receive_would_block(maelys_sys_loop_backend_t backend) {
    (void)backend;
    maelys_sys_socket_t *listener = NULL;
    struct sockaddr_in address;
    socklen_t length = (socklen_t)sizeof(address);
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
    maelys_sys_socket_t *client = NULL;
    maelys_sys_socket_t *accepted = NULL;
    maelys_sys_connect_state_t state;
    unsigned flags = 0;
    uint64_t deadline = 0;
    CHECK(maelys_sys_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &client) ==
        MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_connect_start(client, (const struct sockaddr *)&address,
        length, &state) == MAELYS_SYS_OK);
    CHECK(maelys_sys_deadline_after(1000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_fd_wait(maelys_sys_socket_native_fd(listener),
        MAELYS_SYS_INTEREST_READ, deadline, &flags) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_accept(listener, NULL, NULL, &accepted) == MAELYS_SYS_OK);
    if (state == MAELYS_SYS_CONNECT_IN_PROGRESS) {
        CHECK(maelys_sys_fd_wait(maelys_sys_socket_native_fd(client),
            MAELYS_SYS_INTEREST_WRITE, deadline, &flags) == MAELYS_SYS_OK);
    }
    CHECK(maelys_sys_socket_connect_complete(client) == MAELYS_SYS_OK);
    char byte = 0;
    size_t got = 0;
    CHECK(maelys_sys_socket_receive(accepted, &byte, 1u, &got) == MAELYS_SYS_ERR_WOULD_BLOCK);
    CHECK(got == 0u);
    CHECK(maelys_sys_socket_release(&client) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_release(&accepted) == MAELYS_SYS_OK);
    CHECK(maelys_sys_socket_release(&listener) == MAELYS_SYS_OK);
    return 0;
}

/* A peer's reset seen from the sending side is ERR_RESET on both hosts,
 * whether the first report comes from a bare send or from a wait; once
 * reported, the socket is closed. */
static int reset_on_send(maelys_sys_loop_backend_t backend) {
    (void)backend;
    for (int through_wait = 0; through_wait < 2; ++through_wait) {
        maelys_sys_socket_t *listener = NULL;
        struct sockaddr_in address;
        socklen_t length = (socklen_t)sizeof(address);
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
        maelys_sys_socket_t *client = NULL;
        maelys_sys_socket_t *accepted = NULL;
        maelys_sys_connect_state_t state;
        unsigned flags = 0;
        uint64_t deadline = 0;
        CHECK(maelys_sys_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP, &client) ==
            MAELYS_SYS_OK);
        CHECK(maelys_sys_socket_connect_start(client, (const struct sockaddr *)&address,
            length, &state) == MAELYS_SYS_OK);
        CHECK(maelys_sys_deadline_after(1000, &deadline) == MAELYS_SYS_OK);
        CHECK(maelys_sys_fd_wait(maelys_sys_socket_native_fd(listener),
            MAELYS_SYS_INTEREST_READ, deadline, &flags) == MAELYS_SYS_OK);
        CHECK(maelys_sys_socket_accept(listener, NULL, NULL, &accepted) == MAELYS_SYS_OK);
        if (state == MAELYS_SYS_CONNECT_IN_PROGRESS) {
            CHECK(maelys_sys_fd_wait(maelys_sys_socket_native_fd(client),
                MAELYS_SYS_INTEREST_WRITE, deadline, &flags) == MAELYS_SYS_OK);
        }
        CHECK(maelys_sys_socket_connect_complete(client) == MAELYS_SYS_OK);
        /* The client resets: a close with linger 0 sends RST, not FIN. */
        struct linger abort = {.l_onoff = 1, .l_linger = 0};
        CHECK(setsockopt(maelys_sys_socket_native_fd(client), SOL_SOCKET, SO_LINGER,
            &abort, (socklen_t)sizeof(abort)) == 0);
        CHECK(maelys_sys_socket_release(&client) == MAELYS_SYS_OK);
        CHECK(maelys_sys_fd_wait(maelys_sys_socket_native_fd(accepted),
            MAELYS_SYS_INTEREST_READ, deadline, &flags) == MAELYS_SYS_OK);
        int fd = maelys_sys_socket_native_fd(accepted);
        char byte = 0;
        size_t written = 0;
        errno = 0;
        if (through_wait) {
            CHECK(maelys_sys_socket_send_all_until(fd, &byte, 1u, deadline) ==
                MAELYS_SYS_ERR_RESET);
        } else {
            CHECK(maelys_sys_socket_send_nosigpipe(fd, &byte, 1u, &written) ==
                MAELYS_SYS_ERR_RESET);
        }
        CHECK(errno == ECONNRESET);
        /* The reset was reported once; what remains is a closed socket. */
        CHECK(maelys_sys_socket_send_nosigpipe(fd, &byte, 1u, &written) ==
            MAELYS_SYS_ERR_CLOSED);
        CHECK(maelys_sys_socket_send_all_until(fd, &byte, 1u, deadline) ==
            MAELYS_SYS_ERR_CLOSED);
        CHECK(maelys_sys_socket_release(&accepted) == MAELYS_SYS_OK);
        CHECK(maelys_sys_socket_release(&listener) == MAELYS_SYS_OK);
    }
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
    /* Reading the reset socket is ERR_RESET, never the clean end of stream. */
    {
        char byte = 0;
        size_t got = 0;
        CHECK(maelys_sys_socket_receive(accepted, &byte, 1u, &got) == MAELYS_SYS_ERR_RESET);
    }
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
    CHECK(receive_would_block(backend) == 0);
    CHECK(reset_on_send(backend) == 0);
    CHECK(forked_child_unwatches(backend) == 0);
    CHECK(timer_always_due_and_descriptor(backend) == 0);
    CHECK(timer_and_descriptor_take_turns(backend) == 0);
    CHECK(more_timers_due_than_room(backend) == 0);
    printf("ok - %s backend parity\n", label);
    return 0;
}

int main(void) {
    if (run_backend(MAELYS_SYS_LOOP_POLL, "poll") != 0) return 1;
    if (run_backend(MAELYS_SYS_LOOP_AUTO, "native") != 0) return 1;
    return 0;
}
