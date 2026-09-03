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

cat > "$root/smoke.c" <<'EOF'
#include <maelys/sys.h>
int main(void) {
    maelys_sys_loop_t *loop = 0;
    if (maelys_sys_loop_create(MAELYS_SYS_LOOP_AUTO, &loop) != MAELYS_SYS_OK) {
        return 1;
    }
    return maelys_sys_loop_destroy(&loop) == MAELYS_SYS_OK ? 0 : 2;
}
EOF

${CC:-cc} -std=c11 -Wall -Wextra -Werror -pthread \
    -I"$root/usr/include" "$root/smoke.c" \
    "$root/usr/lib/libmaelys_sys.a" -o "$root/smoke"
"$root/smoke"
printf '%s\n' "install check: ok"
