/*
 * Owned process groups, white box: the host's side compiled into the test
 * with a step it may act at, so that a race the kernel will not produce on
 * demand is played deterministically. The keeper here is a shell script
 * that speaks the protocol by hand, so that what it says and when is the
 * test's to choose.
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif
#define MAELYS_SYS_OWNED_TESTING 1
#include "src/owned.c"

#include <sys/stat.h>
#include <time.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d check failed: %s (errno %d: %s)\n", \
            __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        return 1; \
    } \
} while (0)

static const char *fault_step;
static long fault_sleep_ms;

static void sleep_ms(long ms) {
    struct timespec pause = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
}

/* Acts once at the step armed: here, by letting time pass. */
static int owned_fault(const char *step) {
    if (!fault_step || strcmp(step, fault_step) != 0) return 0;
    fault_step = NULL;
    sleep_ms(fault_sleep_ms);
    return 1;
}

static char root[256];
static char fake[sizeof(root) + 32];
static char self[4096];

static int count_open(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) >= 0) ++count;
    }
    return count;
}

static int write_text(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
    CHECK(fd >= 0);
    size_t length = strlen(text);
    while (length) {
        ssize_t written = write(fd, text, length);
        if (written < 0 && errno == EINTR) continue;
        CHECK(written > 0);
        text += written;
        length -= (size_t)written;
    }
    CHECK(close(fd) == 0);
    return 0;
}

/*
 * A keeper that says HELLO, lets the host's deadline pass, then says in
 * one breath that the program started, that it exited 0 and that the
 * group ended, and waits for the acknowledgement of DONE. rec TYPE A PID
 * writes one record of the protocol: magic "MSOW", protocol 1, the type,
 * then seq and value at zero, the pid, a, and zeros to 64 bytes.
 */
static const char fake_script[] =
    "#!/bin/sh\n"
    "rec() {\n"
    "  printf 'WOSM\\001\\000' >&4\n"
    "  printf \"\\\\$(printf '%03o' \"$1\")\\000\" >&4\n"
    "  printf '\\000\\000\\000\\000\\000\\000\\000\\000' >&4\n"
    "  printf \"\\\\$(printf '%03o' \"$3\")\\000\\000\\000\" >&4\n"
    "  printf \"\\\\$(printf '%03o' \"$2\")\\000\\000\\000\" >&4\n"
    "  i=0; while [ $i -lt 40 ]; do printf '\\000' >&4; i=$((i + 1)); done\n"
    "}\n"
    "rec 1 1 0\n"
    "sleep 1\n"
    "rec 2 0 0\n"
    "rec 3 0 7\n"
    "rec 6 1 7\n"
    "rec 10 7 7\n"
    "dd bs=64 count=1 <&4 >/dev/null 2>&1\n"
    "exit 0\n";

/* The deadline passes with nothing read; by the time of the last look,
 * STARTED and everything up to DONE are in the socket. The start is a
 * success, and the state is the one DONE said, not RUNNING. */
static int test_started_then_done_in_the_last_look(void) {
    int before = count_open();
    char *argv[] = {self, "--child", "exit", "0", NULL};
    maelys_sys_process_options_t program = {.path = self, .argv = argv};
    maelys_sys_owned_options_t options;
    memset(&options, 0, sizeof(options));
    options.keeper_path = fake;
    options.program = &program;
    options.heartbeat_ms = 100;
    options.observe_ms = 1000;
    options.linger_ms = 1000;
    CHECK(maelys_sys_deadline_after(500, &options.start_deadline_ms) == MAELYS_SYS_OK);
    fault_step = "before-last-look";
    fault_sleep_ms = 900;
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    maelys_sys_result_t started = maelys_sys_owned_spawn(&options, &owned);
    CHECK(fault_step == NULL); /* the step was reached */
    CHECK(started == MAELYS_SYS_OK && owned);
    CHECK(maelys_sys_owned_pid(owned) == 7);
    CHECK(maelys_sys_owned_poll(owned, &state) == MAELYS_SYS_OK);
    CHECK(state.phase == MAELYS_SYS_OWNED_ENDED);
    CHECK(state.leader_ended && state.leader.exited && state.leader.exit_code == 0);
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(5000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_owned_wait(owned, deadline, &state) == MAELYS_SYS_OK);
    CHECK(state.phase == MAELYS_SYS_OWNED_ENDED && state.keeper_reaped);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK && !owned);
    CHECK(count_open() == before);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    if (!realpath(argv[0], self)) return 1;
    const char *base = getenv("TMPDIR");
    if (!base || base[0] != '/') base = "/tmp";
    int written = snprintf(root, sizeof(root), "%s/maelys-sys-owned-faults.XXXXXX", base);
    if (written <= 0 || (size_t)written >= sizeof(root) || !mkdtemp(root)) return 1;
    snprintf(fake, sizeof(fake), "%s/late-keeper.sh", root);
    if (write_text(fake, fake_script) != 0) return 1;
    int failed = test_started_then_done_in_the_last_look();
    (void)unlink(fake);
    if (failed) return 1;
    if (rmdir(root) != 0) return 1;
    puts("ok - owned white box: STARTED and DONE read in the last look give a success whose state is DONE's");
    return 0;
}
