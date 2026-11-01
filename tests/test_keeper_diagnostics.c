/* Deterministic kernel outcomes: no real PID is ever signaled here.
 * The real keeper's state machine and wire snapshots are exercised. */
#if defined(__linux__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int fake_kill(pid_t pid, int signo);
static pid_t fake_waitpid(pid_t pid, int *status, int options);
int keeper_test_entry(int argc, char **argv);
#define kill fake_kill
#define waitpid fake_waitpid
#define main keeper_test_entry
#include "src/keeper/keeper.c"
#undef main
#undef waitpid
#undef kill

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)

static int next_error, last_signo, calls, can_reap;
static pid_t last_target;
static int fake_kill(pid_t pid, int signo) {
    last_target = pid;
    last_signo = signo;
    ++calls;
    errno = next_error;
    return next_error ? -1 : 0;
}
static pid_t fake_waitpid(pid_t pid, int *status, int options) {
    if (options != WNOHANG) abort();
    *status = 0;
    return can_reap ? pid : 0;
}
static keeper_t initial(void) {
    keeper_t k;
    memset(&k, 0, sizeof(k));
    k.x = 123;
    k.x_created = k.exec_confirmed = 1;
    k.phase = MAELYS_SYS_OWNED_RUNNING;
    k.observe_ms = 10;
    k.linger_ms = 20;
    calls = 0;
    can_reap = 0;
    return k;
}
static int check_snapshot(const keeper_t *k) {
    CHECK(k->queue_count);
    const owned_record_t *r = &k->queue[(k->queue_head + k->queue_count - 1) % QUEUE_SIZE];
    owned_refusal_wire_t wire;
    memcpy(&wire, r->pad, sizeof(wire));
    CHECK(r->length == k->refusal_serial);
    CHECK(memcmp(&wire, &k->refusal, sizeof(wire)) == 0);
    return 0;
}
static int term_then_dead_leader(void) {
    keeper_t k = initial();
    next_error = 0;
    request_stop(&k, 1, 100, 10);
    CHECK(last_signo == SIGTERM && last_target == -k.x && calls == 1);
    CHECK(!k.refusal_serial && !k.signal_refused);
    k.leader_ended = 1; /* waitid WNOWAIT has observed termination */
    next_error = EPERM;
    kill_group(&k, 20);
    CHECK(last_signo == SIGKILL && calls == 2);
    CHECK(k.refusal_serial == 1 && k.signal_refused);
    CHECK(k.refusal.signo == SIGKILL && k.refusal.error_number == EPERM);
    CHECK(k.refusal.phase == MAELYS_SYS_OWNED_STOPPING);
    CHECK(k.refusal.flags == (OWNED_R_GROUP | OWNED_R_ENDED));
    CHECK(k.refusal.monotonic_ms > 0);
    can_reap = 1;
    next_error = ESRCH;
    reclaim(&k, 21);
    CHECK(last_signo == 0 && calls == 3 && k.leader_reaped);
    CHECK(k.phase == MAELYS_SYS_OWNED_ENDED && k.signal_refused);
    CHECK(k.refusal_serial == 1 && !(k.refusal.flags & OWNED_R_REAPED));
    CHECK(check_snapshot(&k) == 0);
    return 0;
}
static int persistent_refusal(void) {
    keeper_t k = initial();
    next_error = EPERM;
    kill_group(&k, 10);
    reclaim(&k, 20); /* leader cannot be reaped */
    CHECK(k.phase == MAELYS_SYS_OWNED_UNCERTAIN && !k.leader_reaped);
    CHECK(k.refusal.flags == OWNED_R_GROUP);
    reclaim(&k, 40);
    CHECK(k.phase == MAELYS_SYS_OWNED_UNCERTAIN_FINAL);
    CHECK(calls == 1 && k.signal_refused && check_snapshot(&k) == 0);

    /* A dead leader is not grounds for ignoring EPERM on the group. */
    k = initial();
    k.leader_ended = 1;
    kill_group(&k, 10);
    can_reap = 1;
    reclaim(&k, 20); /* probe also denied, despite leader reaped */
    CHECK(calls == 2 && last_signo == 0);
    CHECK(k.refusal_serial == 2 && k.refusal.signo == 0);
    CHECK(k.refusal.flags == (OWNED_R_GROUP | OWNED_R_ENDED | OWNED_R_REAPED));
    CHECK(k.refusal.phase == MAELYS_SYS_OWNED_OBSERVING);
    CHECK(k.phase == MAELYS_SYS_OWNED_UNCERTAIN);
    reclaim(&k, 40);
    CHECK(k.phase == MAELYS_SYS_OWNED_UNCERTAIN_FINAL && check_snapshot(&k) == 0);
    return 0;
}
static int successful_signal_retains_history(void) {
    keeper_t k = initial();
    owned_record_t request = make_record(OWNED_H_SIGNAL);
    request.seq = 1;
    request.a = 1;
    request.value = SIGTERM;
    next_error = EPERM;
    handle_request(&k, &request, 10);
    CHECK(k.signal_refused && k.refusal_serial == 1);
    uint64_t when = k.refusal.monotonic_ms;
    request.seq = 2;
    next_error = 0;
    handle_request(&k, &request, 20);
    CHECK(!k.signal_refused && k.refusal_serial == 1);
    CHECK(k.refusal.monotonic_ms == when && check_snapshot(&k) == 0);
    request.seq = 3;
    request.a = 0;
    next_error = EPERM;
    handle_request(&k, &request, 30);
    CHECK(last_target == k.x && k.refusal_serial == 2 && !k.refusal.flags);
    CHECK(!k.signal_refused); /* process refusal never changes legacy group flag */
    CHECK(check_snapshot(&k) == 0);
    return 0;
}
int main(void) {
    if (term_then_dead_leader() || persistent_refusal() || successful_signal_retains_history()) return 1;
    puts("ok - keeper refusal provenance, historical success, persistent refusals and final cleanup are independent");
    return 0;
}
