#!/bin/sh
set -eu

examples_dir=$1
test "$("$examples_dir/timer-server" 3 1)" = "tick 1
tick 2
tick 3"
test "$("$examples_dir/cross-thread-wakeup")" = "worker complete"
"$examples_dir/tcp-relay" --help >/dev/null

python3 "$(dirname "$0")/test-tcp-relay.py" "$examples_dir/tcp-relay"
printf '%s\n' "examples check: ok"
