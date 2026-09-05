#!/bin/sh
set -eu

root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
temp_base=$(printenv TMPDIR || printf '%s' /tmp)
work=$(mktemp -d "$temp_base/maelys-system-mutations.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

killed=0
host=$(uname -s)

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
    # The build runs without a time limit, so a slow host cannot pass a
    # mutant off as killed; only the tests are bounded, and a hang is a
    # kill of its own kind, said as such.
    if ! make -C "$mutant" BUILD=build/mutant all tests >/dev/null 2>&1; then
        printf '%s\n' "mutation killed by the compiler: $name"
        killed=$((killed + 1))
        return 0
    fi
    status=0
    python3 "$root/scripts/run-with-timeout.py" 60 \
        make -C "$mutant" BUILD=build/mutant test >/dev/null 2>&1 || status=$?
    if test "$status" -eq 0; then
        printf '%s\n' "mutation survived: $name" >&2
        return 1
    fi
    if test "$status" -eq 124; then printf '%s\n' "mutation killed by a hang: $name"
    else printf '%s\n' "mutation killed: $name"; fi
    killed=$((killed + 1))
}

# A fault only the Linux branch of the code can carry.
run_mutant_linux() {
    if test "$host" = Linux; then run_mutant "$@"; else printf '%s\n' "mutation skipped on $host: $1"; fi
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

# Contracts a cold audit of 0.8.0 found unobserved by the suite.
run_mutant step-eintr-reported src/loop.c \
    'if (result == MAELYS_SYS_ERR_OS && errno == EINTR) continue;' 'if (0) continue;'
run_mutant stop-not-sticky src/loop.c \
    'if (atomic_load_explicit(&loop->stopped, memory_order_acquire)) {' 'if (0) {'
run_mutant bounded-read-eintr-reported src/file.c \
    '            if (errno == EINTR) continue;
            return MAELYS_SYS_ERR_OS;' \
    '            if (0) continue;
            return MAELYS_SYS_ERR_OS;'
run_mutant lock-release-keeps-lock src/file.c \
    'if (flock(owned->fd, LOCK_UN) != 0) result = MAELYS_SYS_ERR_OS;' '(void)0;'
run_mutant parent-sync-wrong-directory src/file.c \
    'return maelys_sys_directory_sync(parent);' 'return maelys_sys_directory_sync(".");'
run_mutant_linux thread-name-not-truncated src/thread.c \
    '#define THREAD_NAME_LIMIT 15u' '#define THREAD_NAME_LIMIT 63u'
run_mutant_linux condition-wall-clock src/thread.c \
    'status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);' 'status = 0;'

printf '%s\n' "mutation check: $killed/$killed killed"
