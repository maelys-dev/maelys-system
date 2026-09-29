#!/bin/sh
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
version=$(sed -n '1p' "$root/VERSION")
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || exit 1
header="$root/include/maelys/sys/version.h"
temporary=$(mktemp "$header.XXXXXX")
trap 'rm -f "$temporary"' EXIT HUP INT TERM
sed "s/^#define MAELYS_SYS_VERSION \"[^\"]*\"/#define MAELYS_SYS_VERSION \"$version\"/" \
    "$header" > "$temporary"
chmod 0644 "$temporary"
if ! cmp -s "$header" "$temporary"; then mv "$temporary" "$header"; fi
