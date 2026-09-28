#!/bin/sh
# fdpass-member-check.sh ARCHIVE: the archive carries descriptor passing as
# one member named fdpass.o, and that member stands alone. A consumer may
# extract it with `ar x` from an installed archive, as maelys-egress does for
# the client a confined process links, so the check reads the member itself,
# not the object the build left beside it: exactly one member of that name,
# no undefined symbol of the library or of the thread runtime, and a round
# trip linked against it alone, without -pthread.
set -eu

archive=$1
test -f "$archive" || { printf '%s\n' "fdpass member check: no archive $archive" >&2; exit 1; }
work=$(mktemp -d "${TMPDIR:-/tmp}/maelys-system-fdpass.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
case $archive in /*) ;; *) archive=$(pwd)/$archive ;; esac

members=$(${AR:-ar} t "$archive" | grep -c -x 'fdpass.o' || true)
if test "$members" -ne 1; then
    printf '%s\n' "fdpass member check: $archive holds $members member(s) named fdpass.o, not one" >&2
    exit 1
fi
(cd "$work" && ${AR:-ar} x "$archive" fdpass.o)
if nm -u "$work/fdpass.o" | grep -E 'maelys_sys_|pthread_'; then
    printf '%s\n' "fdpass member check: fdpass.o must stand alone" >&2
    exit 1
fi

include=${FDPASS_INCLUDE:-$(dirname "$0")/../include}
cat > "$work/smoke.c" <<'SMOKE'
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include <maelys/sys/fdpass.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int pair[2], carried[2], received = -1;
    char byte = 0;
    size_t length = 0, count = 0;
    unsigned flags = 0;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0 || pipe(carried) != 0) return 1;
    if (maelys_sys_fd_send(pair[0], "x", 1u, carried[0]) != MAELYS_SYS_OK) return 2;
    if (maelys_sys_fd_receive(pair[1], &byte, 1u, &length, &received, 1u, &count,
            &flags) != MAELYS_SYS_OK) return 3;
    return count == 1u && length == 1u && byte == 'x' && received >= 0 ? 0 : 4;
}
SMOKE
# CFLAGS and LDFLAGS carry a sanitizer when the archive was built with one.
${CC:-cc} -std=c11 -Wall -Wextra -Werror ${CFLAGS:-} -I"$include" \
    "$work/smoke.c" "$work/fdpass.o" ${LDFLAGS:-} -o "$work/smoke"
"$work/smoke"
printf '%s\n' "fdpass member check: ok"
