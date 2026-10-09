/*
 * Processes: start a program with exactly the descriptors named, follow
 * it, stop it within a bound, and tell whether a number still names the
 * process it did.
 *
 * The table is staged before it is applied: every source is duplicated,
 * close-on-exec, above every target, and the targets are made from those
 * duplicates. Applied in order from the sources themselves, {3->4, 4->3}
 * would copy the old 3 twice, measured on both hosts; staged, it is the
 * exchange the caller wrote.
 *
 * macOS starts the program with posix_spawn and POSIX_SPAWN_CLOEXEC_DEFAULT:
 * the program keeps only what the file actions name, and the duplicates,
 * staged in the caller, are what they name. Linux forks: the child stages
 * on its own side, marks every descriptor close-on-exec with close_range
 * (or, on a kernel without it, by walking /proc/self/fd with nothing
 * allocated), makes the targets, and reports a failed exec through a
 * close-on-exec pipe. Between fork and exec only async-signal-safe calls
 * run, which is what makes spawn legal beside other threads.
 *
 * The end of a program is a descriptor: a pidfd on Linux, a kqueue with
 * EVFILT_PROC on macOS. Registering the kqueue on a program that has
 * already ended answers ESRCH, measured, so that case is made readable
 * another way rather than left to a race.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "maelys/sys/process.h"

#include "maelys/sys/clock.h"
#include "maelys/sys/fd.h"
#include "maelys/sys/loop.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#define PROCESS_FORK 1
#elif defined(__APPLE__)
#include <libproc.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#define PROCESS_FORK 0
#else
#error "maelys-system supports Linux and macOS"
#endif

extern char **environ;

#if defined(MAELYS_SYS_PROCESS_TESTING)
static int process_fault(const char *step);
static pid_t process_fault_pid; /* the process the step is about */
#define PROCESS_FAULT(step) (process_fault(step) != 0)
#else
#define PROCESS_FAULT(step) 0
#endif

struct maelys_sys_process {
    pid_t pid;
    int exit_fd;
    atomic_int reaped;
    maelys_sys_process_status_t status;
};

/* The table once checked: targets distinct, sources open, and where the
 * duplicates go, above every target and above 2. */
typedef struct staged_table {
    size_t count;
    int source[MAELYS_SYS_PROCESS_MAX_FDS];
    int target[MAELYS_SYS_PROCESS_MAX_FDS];
    int base;
} staged_table_t;

static maelys_sys_result_t check_options(
    const maelys_sys_process_options_t *options,
    staged_table_t *table) {
    if (!options || !options->path || !options->argv) return MAELYS_SYS_ERR_ARGUMENT;
    if (options->path[0] != '/') return MAELYS_SYS_ERR_ARGUMENT;
    if (options->fd_count > MAELYS_SYS_PROCESS_MAX_FDS) return MAELYS_SYS_ERR_ARGUMENT;
    if (options->fd_count && !options->fds) return MAELYS_SYS_ERR_ARGUMENT;
    unsigned known = MAELYS_SYS_PROCESS_NEW_SESSION | MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP;
    if (options->flags & ~known) return MAELYS_SYS_ERR_ARGUMENT;
    if ((options->flags & known) == known) return MAELYS_SYS_ERR_ARGUMENT;
    table->count = options->fd_count;
    table->base = 3;
    for (size_t index = 0; index < options->fd_count; ++index) {
        int source = options->fds[index].source;
        int target = options->fds[index].target;
        if (source < 0 || target < 0) return MAELYS_SYS_ERR_ARGUMENT;
        if (fcntl(source, F_GETFD) < 0) return MAELYS_SYS_ERR_ARGUMENT;
        for (size_t other = 0; other < index; ++other) {
            if (options->fds[other].target == target) return MAELYS_SYS_ERR_ARGUMENT;
        }
        table->source[index] = source;
        table->target[index] = target;
        if (target >= table->base) table->base = target + 1;
    }
    return MAELYS_SYS_OK;
}

static maelys_sys_result_t start_failure(int error) {
    errno = error;
    return error == ENOENT ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
}

static void fill_status(maelys_sys_process_status_t *status, int raw) {
    memset(status, 0, sizeof(*status));
    if (WIFEXITED(raw)) {
        status->exited = 1;
        status->exit_code = WEXITSTATUS(raw);
    } else if (WIFSIGNALED(raw)) {
        status->signaled = 1;
        status->term_signal = WTERMSIG(raw);
    }
}

/* One look, never a wait: 1 reaped, 0 still running, -1 with errno. */
static int reap_now(maelys_sys_process_t *process) {
    for (;;) {
        int raw = 0;
        pid_t got = waitpid(process->pid, &raw, WNOHANG);
        if (got == process->pid) {
            fill_status(&process->status, raw);
            atomic_store_explicit(&process->reaped, 1, memory_order_release);
            return 1;
        }
        if (got == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

#if PROCESS_FORK

#if defined(SYS_close_range) && !defined(MAELYS_SYS_PROCESS_NO_CLOSE_RANGE)
#define PROCESS_CLOSE_RANGE 1
#else
#define PROCESS_CLOSE_RANGE 0
#endif
#define PROCESS_CLOSE_RANGE_CLOEXEC 4u

/* Every descriptor the child holds becomes close-on-exec, nothing
 * allocated: close_range where the kernel has it, else the names of
 * /proc/self/fd read with getdents64 into a buffer on the stack, else
 * every number up to the limit. The targets are made afterwards. */
static int mark_all_cloexec(rlim_t limit) {
#if PROCESS_CLOSE_RANGE
    if (syscall(SYS_close_range, 0u, ~0u, PROCESS_CLOSE_RANGE_CLOEXEC) == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL) return -1;
#endif
    int directory = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        char buffer[4096];
        for (;;) {
            long length = syscall(SYS_getdents64, directory, buffer, sizeof(buffer));
            if (length < 0) {
                if (errno == EINTR) continue;
                (void)close(directory);
                return -1;
            }
            if (length == 0) break;
            for (long offset = 0; offset < length;) {
                /* struct linux_dirent64: ino, off, reclen (u16 at 16), type, name at 19. */
                const unsigned char *record = (const unsigned char *)buffer + offset;
                unsigned short reclen = (unsigned short)(record[16] | (record[17] << 8));
                const char *name = (const char *)record + 19;
                if (name[0] >= '0' && name[0] <= '9') {
                    int fd = 0;
                    for (const char *digit = name; *digit >= '0' && *digit <= '9'; ++digit) {
                        fd = fd * 10 + (*digit - '0');
                    }
                    if (fd != directory) (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
                }
                if (!reclen) break;
                offset += reclen;
            }
        }
        (void)close(directory);
        return 0;
    }
    for (rlim_t fd = 0; fd < limit && fd <= (rlim_t)INT32_MAX; ++fd) {
        (void)fcntl((int)fd, F_SETFD, FD_CLOEXEC);
    }
    return 0;
}

/* What runs in the child: async-signal-safe calls only, no allocation. A
 * failure is written as its errno to the report pipe and ends the child. */
static void run_child(
    const maelys_sys_process_options_t *options,
    const staged_table_t *table,
    int report_fd,
    rlim_t limit) {
    int staged[MAELYS_SYS_PROCESS_MAX_FDS];
    int error = 0;
    /* The report pipe goes above the targets too, or a target could take
     * its number and the failure of this exec would never be heard. */
    int report = fcntl(report_fd, F_DUPFD_CLOEXEC, table->base);
    if (report < 0) error = errno;
    for (size_t index = 0; !error && index < table->count; ++index) {
        staged[index] = fcntl(table->source[index], F_DUPFD_CLOEXEC, table->base);
        if (staged[index] < 0) error = errno;
    }
    if (!error && mark_all_cloexec(limit) != 0) error = errno;
    for (size_t index = 0; !error && index < table->count; ++index) {
        /* dup2 never marks its result close-on-exec, and the duplicate
         * never has the target's number: the target is made every time. */
        if (dup2(staged[index], table->target[index]) < 0) error = errno;
    }
    if (!error && options->cwd && chdir(options->cwd) != 0) error = errno;
    if (!error && (options->flags & MAELYS_SYS_PROCESS_NEW_SESSION) && setsid() < 0) error = errno;
    if (!error && (options->flags & MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP) &&
        setpgid(0, 0) != 0) {
        error = errno;
    }
    if (!error) {
        (void)execve(options->path, options->argv, options->envp ? options->envp : environ);
        error = errno;
    }
    if (report >= 0) {
        ssize_t written;
        do {
            written = write(report, &error, sizeof(error));
        } while (written < 0 && errno == EINTR);
    }
    _exit(127);
}

static maelys_sys_result_t start_program(
    const maelys_sys_process_options_t *options,
    const staged_table_t *table,
    pid_t *out_pid) {
    int report[2] = {-1, -1};
    if (PROCESS_FAULT("pipe") || pipe2(report, O_CLOEXEC) != 0) return MAELYS_SYS_ERR_OS;
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) limit.rlim_cur = 65536;
    pid_t pid = PROCESS_FAULT("create") ? -1 : fork();
    if (pid < 0) {
        int saved = errno;
        (void)close(report[0]);
        (void)close(report[1]);
        errno = saved;
        return MAELYS_SYS_ERR_OS;
    }
    if (pid == 0) run_child(options, table, report[1], limit.rlim_cur);
    (void)close(report[1]);
    int error = 0;
    ssize_t got;
    do {
        got = read(report[0], &error, sizeof(error));
    } while (got < 0 && errno == EINTR);
    (void)close(report[0]);
    if (got == 0) {
        *out_pid = pid;
        return MAELYS_SYS_OK;
    }
    /* The child ended right after writing: this reap cannot wait long. */
    int raw = 0;
    while (waitpid(pid, &raw, 0) < 0 && errno == EINTR) {}
    return start_failure(got == (ssize_t)sizeof(error) ? error : EIO);
}

static int open_exit_fd(pid_t pid) {
#if defined(SYS_pidfd_open)
    return (int)syscall(SYS_pidfd_open, pid, 0);
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

#else /* posix_spawn */

static maelys_sys_result_t start_program(
    const maelys_sys_process_options_t *options,
    const staged_table_t *table,
    pid_t *out_pid) {
    int staged[MAELYS_SYS_PROCESS_MAX_FDS];
    size_t made = 0;
    int error = 0;
    for (; made < table->count; ++made) {
        staged[made] = fcntl(table->source[made], F_DUPFD_CLOEXEC, table->base);
        if (staged[made] < 0) {
            error = errno ? errno : EBADF;
            break;
        }
    }
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int have_actions = 0, have_attributes = 0;
    if (!error) {
        error = posix_spawn_file_actions_init(&actions);
        have_actions = error == 0;
    }
    if (!error) {
        error = posix_spawnattr_init(&attributes);
        have_attributes = error == 0;
    }
    for (size_t index = 0; !error && index < table->count; ++index) {
        error = posix_spawn_file_actions_adddup2(&actions, staged[index], table->target[index]);
    }
    if (!error && options->cwd) {
        /* The same action under two names: macOS 26 deprecates the _np one. */
#if defined(__MAC_OS_X_VERSION_MIN_REQUIRED) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
        error = posix_spawn_file_actions_addchdir(&actions, options->cwd);
#else
        error = posix_spawn_file_actions_addchdir_np(&actions, options->cwd);
#endif
    }
    if (!error) {
        short flags = POSIX_SPAWN_CLOEXEC_DEFAULT;
        if (options->flags & MAELYS_SYS_PROCESS_NEW_SESSION) flags |= POSIX_SPAWN_SETSID;
        if (options->flags & MAELYS_SYS_PROCESS_NEW_PROCESS_GROUP) {
            flags |= POSIX_SPAWN_SETPGROUP;
            error = posix_spawnattr_setpgroup(&attributes, 0);
        }
        if (!error) error = posix_spawnattr_setflags(&attributes, flags);
    }
    pid_t pid = -1;
    if (!error) {
        error = PROCESS_FAULT("create") ? errno :
            posix_spawn(&pid, options->path, &actions, &attributes, options->argv,
                options->envp ? options->envp : environ);
    }
    if (have_actions) (void)posix_spawn_file_actions_destroy(&actions);
    if (have_attributes) (void)posix_spawnattr_destroy(&attributes);
    for (size_t index = 0; index < made; ++index) (void)close(staged[index]);
    if (error) return start_failure(error);
    *out_pid = pid;
    return MAELYS_SYS_OK;
}

/* A kqueue readable once the process ends. ESRCH means it already has,
 * measured: the kqueue is then made readable by a user event, so that the
 * descriptor says the same thing either way. */
static int open_exit_fd(pid_t pid) {
    int queue = kqueue();
    if (queue < 0) return -1;
    if (fcntl(queue, F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        (void)close(queue);
        errno = saved;
        return -1;
    }
    struct kevent change;
    EV_SET(&change, (uintptr_t)pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
    if (kevent(queue, &change, 1, NULL, 0, NULL) == 0) return queue;
    if (errno != ESRCH) {
        int saved = errno;
        (void)close(queue);
        errno = saved;
        return -1;
    }
    EV_SET(&change, 1, EVFILT_USER, EV_ADD, 0, 0, NULL);
    if (kevent(queue, &change, 1, NULL, 0, NULL) != 0) {
        int saved = errno;
        (void)close(queue);
        errno = saved;
        return -1;
    }
    EV_SET(&change, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    if (kevent(queue, &change, 1, NULL, 0, NULL) != 0) {
        int saved = errno;
        (void)close(queue);
        errno = saved;
        return -1;
    }
    return queue;
}

#endif

maelys_sys_result_t maelys_sys_process_spawn(
    const maelys_sys_process_options_t *options,
    maelys_sys_process_t **out_process) {
    if (out_process) *out_process = NULL;
    if (!out_process) return MAELYS_SYS_ERR_ARGUMENT;
    staged_table_t table;
    maelys_sys_result_t checked = check_options(options, &table);
    if (checked != MAELYS_SYS_OK) return checked;
    maelys_sys_process_t *process = calloc(1, sizeof(*process));
    if (!process) return MAELYS_SYS_ERR_MEMORY;
    maelys_sys_result_t started = start_program(options, &table, &process->pid);
    if (started != MAELYS_SYS_OK) {
        int saved = errno;
        free(process);
        errno = saved;
        return started;
    }
#if defined(MAELYS_SYS_PROCESS_TESTING)
    process_fault_pid = process->pid;
#endif
    if (PROCESS_FAULT("registered-late")) {}
    process->exit_fd = PROCESS_FAULT("exit-fd") ? -1 : open_exit_fd(process->pid);
    if (process->exit_fd < 0) {
        /* The program started and cannot be followed: it is stopped here,
         * so that no process is left behind an error. */
        int saved = errno;
        (void)kill(process->pid, SIGKILL);
        int raw = 0;
        while (waitpid(process->pid, &raw, 0) < 0 && errno == EINTR) {}
        free(process);
        errno = saved;
        return saved == ENOSYS ? MAELYS_SYS_ERR_UNSUPPORTED : MAELYS_SYS_ERR_OS;
    }
    *out_process = process;
    return MAELYS_SYS_OK;
}

pid_t maelys_sys_process_pid(const maelys_sys_process_t *process) {
    return process ? process->pid : -1;
}

int maelys_sys_process_exit_fd(const maelys_sys_process_t *process) {
    return process ? process->exit_fd : -1;
}

static int is_reaped(const maelys_sys_process_t *process) {
    return atomic_load_explicit(&process->reaped, memory_order_acquire);
}

maelys_sys_result_t maelys_sys_process_wait(
    maelys_sys_process_t *process,
    uint64_t deadline_ms,
    maelys_sys_process_status_t *out_status) {
    if (out_status) memset(out_status, 0, sizeof(*out_status));
    if (!process || !out_status || deadline_ms == MAELYS_SYS_DEADLINE_INFINITE) {
        return MAELYS_SYS_ERR_ARGUMENT;
    }
    if (is_reaped(process)) return MAELYS_SYS_ERR_STATE;
    for (;;) {
        int reaped = reap_now(process);
        if (reaped < 0) return MAELYS_SYS_ERR_OS;
        if (reaped) {
            *out_status = process->status;
            return MAELYS_SYS_OK;
        }
        unsigned flags = 0;
        maelys_sys_result_t ready = maelys_sys_fd_wait(process->exit_fd,
            MAELYS_SYS_INTEREST_READ, deadline_ms, &flags);
        if (ready == MAELYS_SYS_ERR_TIMEOUT) {
            /* The program may have ended in the last instant: one look. */
            reaped = reap_now(process);
            if (reaped < 0) return MAELYS_SYS_ERR_OS;
            if (!reaped) return MAELYS_SYS_ERR_TIMEOUT;
            *out_status = process->status;
            return MAELYS_SYS_OK;
        }
        if (ready != MAELYS_SYS_OK) return ready;
    }
}

maelys_sys_result_t maelys_sys_process_signal(maelys_sys_process_t *process, int signo) {
    if (!process) return MAELYS_SYS_ERR_ARGUMENT;
    if (is_reaped(process)) return MAELYS_SYS_ERR_STATE;
    if (kill(process->pid, signo) == 0) return MAELYS_SYS_OK;
    return errno == EINVAL ? MAELYS_SYS_ERR_ARGUMENT : MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_process_terminate(
    maelys_sys_process_t *process,
    uint64_t grace_ms,
    uint64_t force_ms,
    maelys_sys_process_status_t *out_status) {
    if (out_status) memset(out_status, 0, sizeof(*out_status));
    if (!process || !out_status) return MAELYS_SYS_ERR_ARGUMENT;
    if (is_reaped(process)) {
        *out_status = process->status;
        return MAELYS_SYS_OK;
    }
    static const int ladder[2] = {SIGTERM, SIGKILL};
    const uint64_t waits[2] = {grace_ms, force_ms};
    for (int rung = 0; rung < 2; ++rung) {
        int reaped = reap_now(process);
        if (reaped < 0) return MAELYS_SYS_ERR_OS;
        if (reaped) {
            *out_status = process->status;
            return MAELYS_SYS_OK;
        }
        if (kill(process->pid, ladder[rung]) != 0) return MAELYS_SYS_ERR_OS;
        uint64_t deadline = 0;
        maelys_sys_result_t result = maelys_sys_deadline_after(waits[rung], &deadline);
        if (result != MAELYS_SYS_OK) return result;
        result = maelys_sys_process_wait(process, deadline, out_status);
        if (result != MAELYS_SYS_ERR_TIMEOUT) return result;
    }
    return MAELYS_SYS_ERR_TIMEOUT;
}

maelys_sys_result_t maelys_sys_process_release(maelys_sys_process_t **process) {
    if (!process || !*process) return MAELYS_SYS_OK;
    maelys_sys_process_t *handle = *process;
    *process = NULL;
    maelys_sys_result_t result = MAELYS_SYS_OK;
    if (!is_reaped(handle)) {
        int reaped = reap_now(handle);
        if (reaped < 0) result = MAELYS_SYS_ERR_OS;
        else if (!reaped) result = MAELYS_SYS_ERR_STATE;
    }
    int saved = errno;
    (void)close(handle->exit_fd);
    free(handle);
    errno = saved;
    return result;
}

/* --- identity ------------------------------------------------------- */

#if defined(__linux__)

/* A whole small file into a buffer on the stack; -1 with errno. */
static ssize_t read_small(const char *path, char *buffer, size_t capacity) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t length = 0;
    for (;;) {
        ssize_t got = read(fd, buffer + length, capacity - 1 - length);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            int saved = errno;
            (void)close(fd);
            errno = saved;
            return -1;
        }
        if (got == 0 || length + (size_t)got >= capacity - 1) {
            length += (size_t)got;
            break;
        }
        length += (size_t)got;
    }
    (void)close(fd);
    buffer[length] = '\0';
    return (ssize_t)length;
}

static maelys_sys_result_t read_boot(uint64_t *out_boot) {
    char buffer[8192];
    if (read_small("/proc/stat", buffer, sizeof(buffer)) < 0) return MAELYS_SYS_ERR_OS;
    const char *line = buffer;
    while (line) {
        if (strncmp(line, "btime ", 6) == 0) {
            *out_boot = strtoull(line + 6, NULL, 10);
            return MAELYS_SYS_OK;
        }
        line = strchr(line, '\n');
        if (line) ++line;
    }
    errno = EINVAL;
    return MAELYS_SYS_ERR_OS;
}

maelys_sys_result_t maelys_sys_process_identify(
    pid_t pid,
    maelys_sys_process_identity_t *out_identity) {
    if (out_identity) memset(out_identity, 0, sizeof(*out_identity));
    if (pid <= 0 || !out_identity) return MAELYS_SYS_ERR_ARGUMENT;
    char path[64];
    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) <= 0) return MAELYS_SYS_ERR_OS;
    char buffer[4096];
    if (read_small(path, buffer, sizeof(buffer)) < 0) {
        return errno == ENOENT || errno == ESRCH ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    }
    /* The command name, in parentheses, may hold spaces and parentheses:
     * the fields are counted from the last closing one. starttime is field
     * 22 of the file, the 20th after the name. */
    const char *cursor = strrchr(buffer, ')');
    if (!cursor) {
        errno = EINVAL;
        return MAELYS_SYS_ERR_OS;
    }
    ++cursor;
    for (int field = 0; field < 19; ++field) {
        while (*cursor == ' ') ++cursor;
        while (*cursor && *cursor != ' ') ++cursor;
    }
    while (*cursor == ' ') ++cursor;
    if (*cursor < '0' || *cursor > '9') {
        errno = EINVAL;
        return MAELYS_SYS_ERR_OS;
    }
    uint64_t boot = 0;
    maelys_sys_result_t result = read_boot(&boot);
    if (result != MAELYS_SYS_OK) return result;
    out_identity->pid = pid;
    out_identity->birth = strtoull(cursor, NULL, 10);
    out_identity->boot = boot;
    return MAELYS_SYS_OK;
}

static int open_foreign_exit_fd(pid_t pid) {
    return open_exit_fd(pid);
}

#else

maelys_sys_result_t maelys_sys_process_identify(
    pid_t pid,
    maelys_sys_process_identity_t *out_identity) {
    if (out_identity) memset(out_identity, 0, sizeof(*out_identity));
    if (pid <= 0 || !out_identity) return MAELYS_SYS_ERR_ARGUMENT;
    struct proc_bsdinfo info;
    errno = 0;
    int got = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    if (got != (int)sizeof(info)) {
        if (errno == 0) errno = ESRCH;
        return errno == ESRCH ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    }
    struct timeval boot;
    size_t length = sizeof(boot);
    if (sysctlbyname("kern.boottime", &boot, &length, NULL, 0) != 0) return MAELYS_SYS_ERR_OS;
    out_identity->pid = pid;
    out_identity->birth = (uint64_t)info.pbi_start_tvsec * 1000000u + (uint64_t)info.pbi_start_tvusec;
    out_identity->boot = (uint64_t)boot.tv_sec;
    return MAELYS_SYS_OK;
}

static int open_foreign_exit_fd(pid_t pid) {
    int queue = kqueue();
    if (queue < 0) return -1;
    struct kevent change;
    EV_SET(&change, (uintptr_t)pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, NULL);
    if (fcntl(queue, F_SETFD, FD_CLOEXEC) != 0 || kevent(queue, &change, 1, NULL, 0, NULL) != 0) {
        int saved = errno;
        (void)close(queue);
        errno = saved;
        return -1;
    }
    return queue;
}

#endif

maelys_sys_result_t maelys_sys_process_alive(
    const maelys_sys_process_identity_t *identity,
    int *out_alive) {
    if (out_alive) *out_alive = 0;
    if (!identity || !out_alive) return MAELYS_SYS_ERR_ARGUMENT;
    maelys_sys_process_identity_t now;
    maelys_sys_result_t result = maelys_sys_process_identify(identity->pid, &now);
    if (result == MAELYS_SYS_ERR_NOT_FOUND) return MAELYS_SYS_OK;
    if (result != MAELYS_SYS_OK) return result;
    if (PROCESS_FAULT("identify")) now.birth += 1u;
    *out_alive = now.birth == identity->birth && now.boot == identity->boot;
    return MAELYS_SYS_OK;
}

maelys_sys_result_t maelys_sys_process_exit_fd_open(
    const maelys_sys_process_identity_t *identity,
    int *out_fd) {
    if (out_fd) *out_fd = -1;
    if (!identity || !out_fd) return MAELYS_SYS_ERR_ARGUMENT;
    int alive = 0;
    maelys_sys_result_t result = maelys_sys_process_alive(identity, &alive);
    if (result != MAELYS_SYS_OK) return result;
    if (!alive) return MAELYS_SYS_ERR_NOT_FOUND;
    if (PROCESS_FAULT("between")) {}
    int fd = open_foreign_exit_fd(identity->pid);
    if (fd < 0) return errno == ESRCH ? MAELYS_SYS_ERR_NOT_FOUND : MAELYS_SYS_ERR_OS;
    /* The number may have changed hands between the check and the
     * registration: checked again, so that the descriptor never follows a
     * namesake. */
    result = maelys_sys_process_alive(identity, &alive);
    if (result != MAELYS_SYS_OK || !alive) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return result != MAELYS_SYS_OK ? result : MAELYS_SYS_ERR_NOT_FOUND;
    }
    *out_fd = fd;
    return MAELYS_SYS_OK;
}
