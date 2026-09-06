#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
temp_base=$(printenv TMPDIR || printf '%s' /tmp)
work=$(mktemp -d "$temp_base/maelys-system-includes.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

mkdir -p "$work/poison/maelys/sys"
cat >"$work/poison/maelys/sys/result.h" <<'EOF'
#error "an installed maelys header shadowed the checkout"
EOF

make -C "$root" BUILD="$work/build" CPPFLAGS="-I$work/poison" \
    "$work/build/src/result.o" >/dev/null
printf '%s\n' "include precedence check: ok"
