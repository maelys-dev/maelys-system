#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
temp_base=$(printenv TMPDIR || printf '%s' /tmp)
work=$(mktemp -d "$temp_base/maelys-system-mutations.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

# run_mutant NAME FILE OLD NEW: copies the tree, replaces the first OLD by
# NEW in FILE and requires the tests to fail.
run_mutant() {
    name=$1
    file=$2
    old=$3
    new=$4
    mutant="$work/$name"
    mkdir -p "$mutant"
    (cd "$root" && tar --exclude=.git --exclude=build --exclude=dist -cf - .) |
        (cd "$mutant" && tar -xf -)
    python3 - "$mutant/$file" "$old" "$new" <<'PY'
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

run_mutant stale-generation-increment src/loop.c \
    '++generation;' 'generation += 0u;'
run_mutant stale-watch-accepted src/loop.c \
    'slot->active && slot->generation == generation ? slot : NULL;' \
    'slot->active ? slot : NULL;'
run_mutant timer-heap-reversed src/loop.c \
    'loop->timer_heap[parent].deadline_ms <= node.deadline_ms' \
    'loop->timer_heap[parent].deadline_ms >= node.deadline_ms'
run_mutant caller-deadline-wins-over-timer src/loop.c \
    'loop->timer_heap[0].deadline_ms < effective' \
    'loop->timer_heap[0].deadline_ms > effective'
run_mutant timer-token-lost src/loop.c \
    '.token = slot->token,' '.token = 0,'
run_mutant wake-consumed-when-full src/loop.c \
    'if (produced == event_capacity) {' 'if (0) {'
run_mutant compacted-heap-not-rebuilt src/loop.c \
    'heap_sift_down(loop, i);' '(void)i;'
run_mutant freed-watch-slot-not-reused src/loop.c \
    'loop->free_watch = (uint32_t)((size_t)(slot - loop->watches) + 1u);' \
    'slot->next_free = 0;'
run_mutant dead-timer-not-counted src/loop.c \
    '++loop->timer_heap_dead;' '(void)loop->timer_heap_dead;'
run_mutant dead-count-kept-after-compaction src/loop.c \
    'loop->timer_heap_dead = 0;' '(void)loop->timer_heap_dead;'

# File primitives: the faults a cold review injected and the tests now catch.
run_mutant lock-path-not-rechecked src/file.c \
    'named.st_dev != after.st_dev || named.st_ino != after.st_ino' '0'
run_mutant lock-not-verified-after-flock src/file.c \
    'result = verify_descriptor(fd, expectations, NULL, NULL, &after);' \
    'result = (FAULT("fstat") || fstat(fd, &after)) ? MAELYS_SYS_ERR_OS : MAELYS_SYS_OK;'
run_mutant lock-busy-reported-as-os-error src/file.c \
    'result = errno == EWOULDBLOCK ? MAELYS_SYS_ERR_BUSY : MAELYS_SYS_ERR_OS;' \
    'result = MAELYS_SYS_ERR_OS;'
run_mutant publish-exists-reported-as-os-error src/file.c \
    '            case EEXIST:
            case ENOTEMPTY:' '            case ENOTEMPTY:'
run_mutant nonblock-left-on-descriptor src/file.c \
    'current & ~O_NONBLOCK' 'current'
run_mutant bounded-read-ignores-overflow src/file.c \
    'if (got > 0) return MAELYS_SYS_ERR_CAPACITY;' \
    'if (got > 0) { *out_size = filled; return MAELYS_SYS_OK; }'
run_mutant final-mode-before-content src/file.c \
    '    if (write_all(fd, bytes, length) != 0 ||
        (FAULT("fchmod") || fchmod(fd, final_mode) != 0)) {' \
    '    if ((FAULT("fchmod") || fchmod(fd, final_mode) != 0) ||
        write_all(fd, bytes, length) != 0) {'
run_mutant exclusive-file-created-readable src/file.c \
    'O_CLOEXEC | O_NOFOLLOW, 0600);' 'O_CLOEXEC | O_NOFOLLOW, 0644);'
run_mutant removal-ignores-identity src/file.c \
    'now.st_dev != identity->device || now.st_ino != identity->inode ||' '0 ||'

printf '%s\n' "mutation check: 19/19 killed"
