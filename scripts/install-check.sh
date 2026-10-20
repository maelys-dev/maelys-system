#!/bin/sh
set -eu

root=$(mktemp -d "${TMPDIR:-/tmp}/maelys-system-install.XXXXXX")
cleanup() {
    rm -rf "$root"
}
trap cleanup EXIT HUP INT TERM

make install DESTDIR="$root" PREFIX=/usr
test -f "$root/usr/lib/libmaelys_sys.a"
test -f "$root/usr/include/maelys/sys.h"
test -f "$root/usr/include/maelys/sys/loop.h"
test -f "$root/usr/lib/pkgconfig/maelys-sys.pc"
test -f "$root/usr/include/maelys/sys/fdpass.h"
test -f "$root/usr/include/maelys/sys/dirwatch.h"
test -f "$root/usr/include/maelys/sys/owned.h"
test -x "$root/usr/libexec/maelys-sys/keeper"
# What a consumer extracts from an installed archive, read from that archive.
FDPASS_INCLUDE="$root/usr/include" sh "$(dirname "$0")/fdpass-member-check.sh" \
    "$root/usr/lib/libmaelys_sys.a"

cat > "$root/smoke.c" <<'EOF'
#define _POSIX_C_SOURCE 200809L
#include <maelys/sys.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
    maelys_sys_loop_t *loop = 0;
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) != MAELYS_SYS_OK) return 1;
    if (maelys_sys_loop_destroy(&loop) != MAELYS_SYS_OK) return 2;
    /* The component added last, through what was installed: a directory
     * watched, one member created, that one change reported. */
    maelys_sys_dirwatch_t *dirwatch = 0;
    maelys_sys_dirwatch_entry_t entry = 0;
    maelys_sys_dirwatch_change_t change;
    size_t count = 0;
    uint64_t deadline = 0;
    unsigned ready = 0;
    if (mkdir("watched", 0700) != 0) return 3;
    if (maelys_sys_dirwatch_create(1, &dirwatch) != MAELYS_SYS_OK) return 4;
    if (maelys_sys_dirwatch_add(dirwatch, "watched", 7, &entry) != MAELYS_SYS_OK) return 5;
    int fd = open("watched/member", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || close(fd) != 0) return 6;
    if (maelys_sys_deadline_after(3000, &deadline) != MAELYS_SYS_OK) return 7;
    if (maelys_sys_fd_wait(maelys_sys_dirwatch_fd(dirwatch), MAELYS_SYS_INTEREST_READ,
            deadline, &ready) != MAELYS_SYS_OK) return 8;
    if (maelys_sys_dirwatch_poll(dirwatch, &change, 1, &count) != MAELYS_SYS_OK) return 9;
    if (count != 1 || change.token != 7 || change.entry != entry ||
        change.flags != MAELYS_SYS_DIRWATCH_CHANGED) return 10;
    if (maelys_sys_dirwatch_destroy(&dirwatch) != MAELYS_SYS_OK) return 11;
    /* A program started with no descriptor at all, followed, reaped. */
    char *program_argv[] = {"sh", "-c", "exit 3", 0};
    maelys_sys_process_options_t options = {.path = "/bin/sh", .argv = program_argv};
    maelys_sys_process_t *process = 0;
    maelys_sys_process_status_t status;
    if (maelys_sys_process_spawn(&options, &process) != MAELYS_SYS_OK) return 12;
    if (maelys_sys_deadline_after(3000, &deadline) != MAELYS_SYS_OK) return 13;
    if (maelys_sys_process_wait(process, deadline, &status) != MAELYS_SYS_OK) return 14;
    if (!status.exited || status.exit_code != 3) return 15;
    if (maelys_sys_process_release(&process) != MAELYS_SYS_OK) return 16;
    /* The same program as the leader of an owned group, through the
     * installed keeper named on the command line. */
    maelys_sys_owned_options_t owned_options = {.keeper_path = argv[1], .program = &options,
        .flags = MAELYS_SYS_OWNED_END_WITH_LEADER};
    maelys_sys_owned_t *owned = 0;
    maelys_sys_owned_state_t state;
    if (argc < 2) return 17;
    if (maelys_sys_deadline_after(30000, &owned_options.start_deadline_ms) != MAELYS_SYS_OK) return 18;
    if (maelys_sys_owned_spawn(&owned_options, &owned) != MAELYS_SYS_OK) return 19;
    if (maelys_sys_deadline_after(10000, &deadline) != MAELYS_SYS_OK) return 20;
    if (maelys_sys_owned_wait(owned, deadline, &state) != MAELYS_SYS_OK) return 21;
    if (state.phase != MAELYS_SYS_OWNED_ENDED || !state.leader.exited || state.leader.exit_code != 3) return 22;
    return maelys_sys_owned_release(&owned) == MAELYS_SYS_OK ? 0 : 23;
}
EOF

${CC:-cc} -std=c11 -Wall -Wextra -Werror -pthread \
    -I"$root/usr/include" "$root/smoke.c" \
    "$root/usr/lib/libmaelys_sys.a" -o "$root/smoke"
# In the scratch root: the smoke program makes the directory it watches.
(cd "$root" && ./smoke "$root/usr/libexec/maelys-sys/keeper")
printf '%s\n' "install check: ok"
