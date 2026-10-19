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

/* A file the step creates as it is reached, for a scripted keeper that
 * waits for it; empty: none. */
static char fault_signal_path[512];

/* Acts once at the step armed: creates the signal file if one is named,
 * then lets time pass. */
static int owned_fault(const char *step) {
    if (!fault_step || strcmp(step, fault_step) != 0) return 0;
    fault_step = NULL;
    if (fault_signal_path[0]) {
        int fd = open(fault_signal_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) (void)close(fd);
    }
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
 * A keeper that says HELLO, waits until the host has passed its deadline
 * and reached the step before its last look (the step creates a file the
 * script waits for), then says in one breath that the program started,
 * that it exited 0 and that the group ended, and waits for the
 * acknowledgement of DONE. The order holds whatever the script's speed. rec TYPE A PID
 * writes one record of the protocol: magic "MSOW", protocol 1, the type,
 * then seq and value at zero, the pid, a, and zeros to 64 bytes.
 */
static const char fake_script_head[] =
    "#!/bin/sh\n"
    "rec() {\n"
    "  printf 'WOSM\\001\\000' >&4\n"
    "  printf \"\\\\$(printf '%03o' \"$1\")\\000\" >&4\n"
    "  printf '\\000\\000\\000\\000\\000\\000\\000\\000' >&4\n"
    "  printf \"\\\\$(printf '%03o' \"$3\")\\000\\000\\000\" >&4\n"
    "  printf \"\\\\$(printf '%03o' \"$2\")\\000\\000\\000\" >&4\n"
    "  i=0; while [ $i -lt 40 ]; do printf '\\000' >&4; i=$((i + 1)); done\n"
    "}\n"
    "rec 1 1 0\n";
/* then: wait for the file the host's step creates, and say the rest */
static const char fake_script_tail[] =
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
    /* HELLO within three seconds; the rest once the host is at its last
     * look, which then waits a second for it to arrive */
    CHECK(maelys_sys_deadline_after(3000, &options.start_deadline_ms) == MAELYS_SYS_OK);
    fault_step = "before-last-look";
    fault_sleep_ms = 1000;
    snprintf(fault_signal_path, sizeof(fault_signal_path), "%s/go", root);
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
    CHECK(unlink(fault_signal_path) == 0);
    fault_signal_path[0] = '\0';
    return 0;
}

static char split_keeper[4096];
static char never_reap_keeper[4096];

/* A keeper of the test build, beside this test. */
static void beside(char *out, size_t size, const char *name) {
    const char *slash = strrchr(self, '/');
    snprintf(out, size, "%.*s/%s", (int)(slash ? slash - self : 0), self, name);
}

/* One bare exec, so that the first start of a freshly built binary is not
 * held by the host's scanning when a short deadline is under test. It
 * exits at once, its descriptors 3 and 4 missing. */
static int warm_up(const char *path) {
    char *argv[] = {(char *)path, NULL};
    maelys_sys_process_options_t options = {.path = path, .argv = argv};
    maelys_sys_process_t *process = NULL;
    maelys_sys_process_status_t status;
    uint64_t deadline = 0;
    CHECK(maelys_sys_process_spawn(&options, &process) == MAELYS_SYS_OK);
    CHECK(maelys_sys_deadline_after(60000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_wait(process, deadline, &status) == MAELYS_SYS_OK);
    CHECK(maelys_sys_process_release(&process) == MAELYS_SYS_OK);
    return 0;
}

static maelys_sys_owned_options_t sleeper_options(const char *keeper, maelys_sys_process_options_t *program,
    char **argv, uint64_t start_ms) {
    argv[0] = "/bin/sleep";
    argv[1] = "30";
    argv[2] = NULL;
    memset(program, 0, sizeof(*program));
    program->path = argv[0];
    program->argv = argv;
    maelys_sys_owned_options_t options;
    memset(&options, 0, sizeof(options));
    options.keeper_path = keeper;
    options.program = program;
    options.heartbeat_ms = 100;
    options.observe_ms = 300;
    options.linger_ms = 1050;
    (void)maelys_sys_deadline_after(start_ms, &options.start_deadline_ms);
    return options;
}

/* STARTED's first byte goes before the start deadline, the rest after it,
 * and the host's last look comes after the rest. The keeper must not take
 * the deadline for an abandon: the host reads the record whole and the
 * start is a success on both sides, the program left running. */
static int test_split_started_is_validated(void) {
    int before = count_open();
    char *argv[3];
    maelys_sys_process_options_t program;
    /* Twenty seconds: the keeper of this test holds the rest of STARTED
     * until the deadline whatever its length, and on a host where an exec
     * may stay in _dyld_start for over ten seconds (measured on one
     * machine) HELLO must still come in time. */
    maelys_sys_owned_options_t options = sleeper_options(split_keeper, &program, argv, 20000);
    fault_step = "before-last-look";
    fault_sleep_ms = 600;
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    uint64_t before_spawn = 0, after_spawn = 0;
    (void)maelys_sys_monotonic_ms(&before_spawn);
    maelys_sys_result_t started = maelys_sys_owned_spawn(&options, &owned);
    (void)maelys_sys_monotonic_ms(&after_spawn);
    if (fault_step != NULL) {
        fprintf(stderr, "split: the last look was not reached: result %d after %llu ms, handle %p\n", (int)started,
            (unsigned long long)(after_spawn - before_spawn), (void *)owned);
    }
    CHECK(fault_step == NULL); /* the deadline did pass with STARTED unread */
    CHECK(started == MAELYS_SYS_OK && owned);
    CHECK(maelys_sys_owned_poll(owned, &state) == MAELYS_SYS_OK);
    CHECK(state.phase == MAELYS_SYS_OWNED_RUNNING && !state.leader_ended);
    sleep_ms(300);
    CHECK(maelys_sys_owned_poll(owned, &state) == MAELYS_SYS_OK);
    CHECK(state.phase == MAELYS_SYS_OWNED_RUNNING && !state.leader_ended);
    uint32_t stop = 0;
    CHECK(maelys_sys_owned_stop(owned, 1000, &stop) == MAELYS_SYS_OK);
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(10000, &deadline) == MAELYS_SYS_OK);
    if (maelys_sys_owned_wait(owned, deadline, &state) != MAELYS_SYS_OK) {
        fprintf(stderr, "split: no final state: phase %d leader_ended %d final? reaped %d elsewhere %d "
            "from_exit %d truncated %d want_write %d, keeper %ld, keeper_fd %d, ctl %d\n", (int)state.phase,
            state.leader_ended, state.keeper_reaped, state.keeper_reaped_elsewhere, state.state_from_exit_code,
            state.truncated_final, state.want_write, (long)owned->keeper, owned->keeper_fd, owned->ctl);
        struct pollfd ready = {owned->keeper_fd, POLLIN, 0};
        int readable = poll(&ready, 1, 0);
        int raw = 0;
        pid_t reaped = waitpid(owned->keeper, &raw, WNOHANG);
        int reap_errno = errno;
        fprintf(stderr, "split: keeper_fd poll %d revents %x; waitpid(keeper, WNOHANG) = %ld errno %d\n", readable,
            ready.revents, (long)reaped, reap_errno);
        char command[160];
        snprintf(command, sizeof(command), "ps -o pid,ppid,stat,etime,command -p %ld >&2", (long)owned->keeper);
        int listed = system(command);
        (void)listed;
#if defined(__APPLE__)
        snprintf(command, sizeof(command), "sample %ld 1 2>/dev/null | sed -n '/Call graph/,/Total number/p' | head -30 >&2",
            (long)owned->keeper);
        listed = system(command);
        (void)listed;
#endif
        return 1;
    }
    CHECK(state.phase == MAELYS_SYS_OWNED_ENDED);
    CHECK(state.leader.signaled && state.leader.term_signal == SIGTERM);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
    CHECK(count_open() == before);
    return 0;
}

/* The leader killed and never reapable: RECLAIMING, then UNCERTAIN with
 * the leader unreaped, then the end of the linger, at its own deadline,
 * with no turn of the keeper's loop spent waiting for nothing. linger_ms
 * is 1050 so that its end falls between two reaping attempts. */
static int test_linger_with_leader_unreaped(void) {
    int before = count_open();
    char here[4096];
    CHECK(getcwd(here, sizeof(here)) != NULL);
    char stats[sizeof(root) + 32];
    snprintf(stats, sizeof(stats), "%s/keeper-stats", root);
    (void)unlink(stats);
    /* the keeper writes its count only into a file that exists */
    if (write_text(stats, "") != 0) return 1;
    char *argv[3];
    maelys_sys_process_options_t program;
    maelys_sys_owned_options_t options = sleeper_options(never_reap_keeper, &program, argv, 30000);
    maelys_sys_owned_t *owned = NULL;
    maelys_sys_owned_state_t state;
    /* the keeper leaves its count in its working directory, the host's */
    CHECK(chdir(root) == 0);
    maelys_sys_result_t started = maelys_sys_owned_spawn(&options, &owned);
    CHECK(chdir(here) == 0);
    CHECK(started == MAELYS_SYS_OK);
    uint32_t stop = 0;
    uint64_t asked = 0;
    CHECK(maelys_sys_monotonic_ms(&asked) == MAELYS_SYS_OK);
    CHECK(maelys_sys_owned_stop(owned, 100, &stop) == MAELYS_SYS_OK);
    uint64_t deadline = 0;
    CHECK(maelys_sys_deadline_after(10000, &deadline) == MAELYS_SYS_OK);
    CHECK(maelys_sys_owned_wait(owned, deadline, &state) == MAELYS_SYS_OK);
    uint64_t done = 0;
    CHECK(maelys_sys_monotonic_ms(&done) == MAELYS_SYS_OK);
    CHECK(state.phase == MAELYS_SYS_OWNED_UNCERTAIN_FINAL);
    CHECK(state.leader_ended && state.leader_unreaped);
    /* observe 300 + linger 1050 after the leader's end, with room */
    CHECK(done - asked >= 1350 && done - asked < 3000);
    FILE *file = fopen(stats, "r");
    CHECK(file != NULL);
    long idle = -1;
    int read_count = fscanf(file, "idle_turns %ld", &idle);
    fclose(file);
    CHECK(read_count == 1);
    fprintf(stderr, "note: %ld idle turns of the keeper's loop over RECLAIMING and UNCERTAIN\n", idle);
    CHECK(idle >= 0 && idle <= 3);
    CHECK(unlink(stats) == 0);
    CHECK(maelys_sys_owned_release(&owned) == MAELYS_SYS_OK);
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
    char script[4096];
    snprintf(script, sizeof(script), "%swhile [ ! -e '%s/go' ]; do sleep 0.02; done\n%s", fake_script_head, root,
        fake_script_tail);
    if (write_text(fake, script) != 0) return 1;
    beside(split_keeper, sizeof(split_keeper), "keeper_split_started");
    beside(never_reap_keeper, sizeof(never_reap_keeper), "keeper_never_reap");
    if (warm_up(split_keeper) || warm_up(never_reap_keeper)) return 1;
    int failed = test_started_then_done_in_the_last_look() || test_split_started_is_validated() ||
        test_linger_with_leader_unreaped();
    (void)unlink(fake);
    if (failed) return 1;
    if (rmdir(root) != 0) return 1;
    puts("ok - owned white box: STARTED and DONE read in the last look give a success whose state is DONE's");
    puts("ok - owned white box: a STARTED begun before the deadline and finished after it is a success on both sides");
    puts("ok - owned white box: a leader never reaped ends in UNCERTAIN_FINAL at the linger's own deadline, without idle turns");
    return 0;
}
