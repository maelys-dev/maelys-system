#!/bin/sh
set -eu

examples_dir=$1
test "$("$examples_dir/timer-server" 3 1)" = "tick 1
tick 2
tick 3"
test "$("$examples_dir/cross-thread-wakeup")" = "worker complete"
"$examples_dir/tcp-relay" --help >/dev/null

python3 "$(dirname "$0")/test-tcp-relay.py" "$examples_dir/tcp-relay"

# directory-watch: it says what it watches after the add, a member is
# created, and it rereads once and leaves.
watched=$(mktemp -d "${TMPDIR:-/tmp}/maelys-system-example.XXXXXX")
trap 'rm -rf "$watched" "$watched.out"' EXIT HUP INT TERM
python3 "$(dirname "$0")/run-with-timeout.py" 20 \
    "$examples_dir/directory-watch" 1 "$watched" >"$watched.out" &
watcher=$!
tries=0
until grep -q '^watching ' "$watched.out" 2>/dev/null; do
    tries=$((tries + 1))
    test "$tries" -lt 100
    sleep 0.1
done
: >"$watched/member"
wait "$watcher"
test "$(cat "$watched.out")" = "watching $watched: 0 entries
reread $watched: 1 entries"

# process-launch: a program that writes and ends, then one that is stopped
# by the ladder when the grace runs out.
test "$("$examples_dir/process-launch" 5000 /bin/sh -c 'echo hello; echo there >&2; exit 4')" = "out: hello
out: there
ended: exit 4"
test "$("$examples_dir/process-launch" 400 /bin/sh -c 'trap "" TERM; exec sleep 30')" = "stopped: signal 9"
printf '%s\n' "examples check: ok"
