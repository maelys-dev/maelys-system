#!/bin/sh
# Builds the product the way a consumer does, with `make all` into a BUILD of
# its own and its own CFLAGS: several makes at once on one BUILD, as a
# consumer's mutation gate runs them; KEEPER_PATH fixed under two prefixes,
# which rebuilds nothing; and, with clang, a fuzzer's flags, which the
# keeper must not take for a fuzzer of its own.
#
#   sh scripts/consumer-build-check.sh CC
set -eu

cc=${1:-cc}
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
temp_base=$(printenv TMPDIR || printf '%s' /tmp)
work=$(mktemp -d "$temp_base/maelys-system-consumer.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

consumer_make() {
    make -s -C "$root" BUILD="$1" CC="$cc" CPPFLAGS= CFLAGS="$2" all
}

# Several makes of all at once on one BUILD already built: none rewrites
# what another is reading.
consumer_make "$work/shared" '-O2 -g' >/dev/null
failed=0
for round in 1 2 3 4 5; do
    pids=
    for make_index in 1 2 3 4 5 6; do
        consumer_make "$work/shared" '-O2 -g' >"$work/make-$round-$make_index.log" 2>&1 &
        pids="$pids $!"
    done
    for pid in $pids; do
        wait "$pid" || failed=$((failed + 1))
    done
done
if test "$failed" -ne 0; then
    cat "$work"/make-*.log >&2
    printf '%s\n' "consumer build check: $failed of 30 concurrent makes failed" >&2
    exit 1
fi

# A consumer that links the archive and installs no keeper fixes
# KEEPER_PATH: two builds under two prefixes then rebuild nothing, the
# path given is the one the library carries, and it reaches a nested make.
fixed=/usr/local/libexec/maelys-sys/keeper
make -s -C "$root" BUILD="$work/prefix" CC="$cc" CPPFLAGS= CFLAGS='-O2 -g' PREFIX=/opt/one KEEPER_PATH="$fixed" all >/dev/null
sleep 1
: >"$work/prefix.mark"
sleep 1
make -s -C "$root" BUILD="$work/prefix" CC="$cc" CPPFLAGS= CFLAGS='-O2 -g' PREFIX=/opt/two KEEPER_PATH="$fixed" all >/dev/null
if test -n "$(find "$work/prefix/lib/libmaelys_sys.a" "$work/prefix/src/owned.o" -newer "$work/prefix.mark")"; then
    printf '%s\n' "consumer build check: another PREFIX rebuilt the archive with KEEPER_PATH fixed" >&2
    exit 1
fi
test "$(cat "$work/prefix/keeper-path")" = "$fixed"
grep -q "$fixed" "$work/prefix/src/owned.o"
printf 'all:\n\t@$(MAKE) -s -C %s BUILD=%s CC=%s CPPFLAGS= all\n' "$root" "$work/nested" "$cc" >"$work/outer.mk"
make -s -f "$work/outer.mk" PREFIX=/opt/three KEEPER_PATH=/fixed/elsewhere/keeper >/dev/null
test "$(cat "$work/nested/keeper-path")" = /fixed/elsewhere/keeper

# A fuzzer's flags, where the compiler has libFuzzer: the library and the
# keeper are instrumented, and the keeper links without a fuzzer's main.
if "$cc" --version 2>/dev/null | grep -q clang &&
    printf 'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n) { (void)d; (void)n; return 0; }\n' |
        "$cc" -x c -fsanitize=fuzzer -o "$work/probe" - >/dev/null 2>&1; then
    consumer_make "$work/fuzz" '-O1 -g -fsanitize=fuzzer,address,undefined' >/dev/null
    status=0
    "$work/fuzz/libexec/maelys-sys/keeper" </dev/null >/dev/null 2>&1 || status=$?
    # 64: started, and refused for want of its descriptors
    test "$status" -eq 64
    printf '%s\n' "consumer build check: ok (30 concurrent makes, KEEPER_PATH fixed, a fuzzer's flags)"
else
    printf '%s\n' "consumer build check: ok (30 concurrent makes, KEEPER_PATH fixed; no libFuzzer with $cc)"
fi
