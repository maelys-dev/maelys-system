#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
temp_base=$(printenv TMPDIR || printf '%s' /tmp)
work=$(mktemp -d "$temp_base/maelys-system-mutations.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

run_mutant() {
    name=$1
    old=$2
    new=$3
    mutant="$work/$name"
    mkdir -p "$mutant"
    (cd "$root" && tar --exclude=.git --exclude=build --exclude=dist -cf - .) |
        (cd "$mutant" && tar -xf -)
    python3 - "$mutant/src/loop.c" "$old" "$new" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
source = path.read_text()
old, new = sys.argv[2], sys.argv[3]
if source.count(old) < 1:
    raise SystemExit("mutation anchor missing: " + old)
path.write_text(source.replace(old, new, 1))
PY
    if python3 "$root/scripts/run-with-timeout.py" 30 \
        make -C "$mutant" BUILD=build/mutant test >/dev/null 2>&1; then
        printf '%s\n' "mutation survived: $name" >&2
        return 1
    fi
    printf '%s\n' "mutation killed: $name"
}

run_mutant stale-generation-increment \
    '++generation;' 'generation += 0u;'
run_mutant stale-watch-accepted \
    'slot->active && slot->generation == generation ? slot : NULL;' \
    'slot->active ? slot : NULL;'
run_mutant timer-heap-reversed \
    'loop->timer_heap[parent].deadline_ms <= node.deadline_ms' \
    'loop->timer_heap[parent].deadline_ms >= node.deadline_ms'
run_mutant caller-deadline-wins-over-timer \
    'loop->timer_heap[0].deadline_ms < effective' \
    'loop->timer_heap[0].deadline_ms > effective'
run_mutant timer-token-lost \
    '.token = slot->token,' '.token = 0,'
run_mutant wake-consumed-when-full \
    'if (produced == event_capacity) {' 'if (0) {'
run_mutant compacted-heap-not-rebuilt \
    'heap_sift_down(loop, i);' '(void)i;'
run_mutant freed-watch-slot-not-reused \
    'loop->free_watch = (uint32_t)((size_t)(slot - loop->watches) + 1u);' \
    'slot->next_free = 0;'
run_mutant dead-timer-not-counted \
    '++loop->timer_heap_dead;' '(void)loop->timer_heap_dead;'
run_mutant dead-count-kept-after-compaction \
    'loop->timer_heap_dead = 0;' '(void)loop->timer_heap_dead;'

printf '%s\n' "mutation check: 10/10 killed"
