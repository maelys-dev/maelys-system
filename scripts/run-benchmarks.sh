#!/bin/sh
set -eu

build=$1
iterations=$(printenv BENCH_ITERATIONS || printf '%s' 10000)
output=$(printenv BENCH_OUTPUT || printf '%s' "$build/benchmarks/results.csv")
compiler=$(printenv CC || printf '%s' cc)
mkdir -p "$(dirname "$output")"
printf '%s\n' "implementation,workload,iterations,elapsed_ns,ns_per_operation" >"$output"
"$build/benchmarks/reactor-maelys" "$iterations" >>"$output"

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libevent; then
    # shellcheck disable=SC2046
    "$compiler" -O2 -std=c11 -Wall -Wextra -Werror \
        $(pkg-config --cflags libevent) benchmarks/reactor_libevent.c \
        $(pkg-config --libs libevent) -o "$build/benchmarks/reactor-libevent"
    "$build/benchmarks/reactor-libevent" "$iterations" >>"$output"
else
    printf '%s\n' "benchmark: libevent unavailable; baseline retained" >&2
fi

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libev; then
    # shellcheck disable=SC2046
    "$compiler" -O2 -std=c11 -Wall -Wextra -Werror \
        $(pkg-config --cflags libev) benchmarks/reactor_libev.c \
        $(pkg-config --libs libev) -o "$build/benchmarks/reactor-libev"
    "$build/benchmarks/reactor-libev" "$iterations" >>"$output"
else
    printf '%s\n' "benchmark: libev unavailable; baseline retained" >&2
fi
cat "$output"
