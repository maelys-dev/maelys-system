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
    # kill of its own kind, said as such. The bound is three times what the
    # suite takes on the slowest host measured (39 s on macOS at 0.13.0),
    # so that a loaded host does not pass a survivor off as a hang.
    if ! make -C "$mutant" BUILD=build/mutant all tests >/dev/null 2>&1; then
        printf '%s\n' "mutation killed by the compiler: $name"
        killed=$((killed + 1))
        return 0
    fi
    status=0
    python3 "$root/scripts/run-with-timeout.py" 120 \
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

# A fault only the macOS kernel makes observable.
run_mutant_darwin() {
    if test "$host" = Darwin; then run_mutant "$@"; else printf '%s\n' "mutation skipped on $host: $1"; fi
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
run_mutant due-timer-skips-the-descriptors src/loop.c \
    'if (room) {' 'if (room && !due) {'
run_mutant timers-keep-no-place src/loop.c \
    'if (due && timers_first) room -= due < room ? due : room;' \
    'if (0) room -= due < room ? due : room;'
run_mutant timers-always-first src/loop.c \
    'if (due) loop->timers_first = !timers_first;' \
    'if (due) loop->timers_first = 1;'
run_mutant descriptors-always-first src/loop.c \
    'if (due) loop->timers_first = !timers_first;' \
    'if (due) loop->timers_first = 0;'

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
    'result = sync_descriptor(destination_entry.parent_fd);' \
    'result = sync_descriptor(source_entry.parent_fd);'
run_mutant parent-sync-skipped src/file.c \
    'result = sync_descriptor(destination_entry.parent_fd);' 'result = MAELYS_SYS_OK;'
# Descriptor passing (0.10): what the two consumers' hand-written copies
# got wrong or never checked, and what makes the hosts answer alike.
run_mutant fdpass-surplus-left-open src/fdpass.c \
    'close_received(descriptors + kept, count - kept);' '(void)0;'
run_mutant fdpass-fcntl-failure-leaks src/fdpass.c \
    '                close_received(descriptors, kept);' '                (void)0;'
run_mutant fdpass-peer-gone-reported-as-os-error src/fdpass.c \
    '            return MAELYS_SYS_ERR_CLOSED;' '            return MAELYS_SYS_ERR_OS;'
run_mutant fdpass-full-queue-reported-as-os-error src/fdpass.c \
    '        case ENOBUFS:
            return MAELYS_SYS_ERR_WOULD_BLOCK;' '        case ENOBUFS:
            return MAELYS_SYS_ERR_OS;'
run_mutant fdpass-socket-type-unchecked src/fdpass.c \
    '        type != SOCK_DGRAM) {' '        0) {'
run_mutant fdpass-cloexec-not-set src/fdpass.c \
    'F_SETFD, descriptor_flags | FD_CLOEXEC)' 'F_SETFD, descriptor_flags)'
run_mutant fdpass-empty-datagram-called-truncated src/fdpass.c \
    '(!capacity && received > 0)' '(!capacity)'
run_mutant fdpass-fcntl-failure-keeps-earlier src/fdpass.c \
    '                close_received(descriptors, kept);' \
    '                close_received(descriptors + index, kept - index);'
run_mutant fdpass-setfd-failure-ignored src/fdpass.c \
    '            if (failed) {' '            if (failed && descriptor_flags < 0) {'
run_mutant fdpass-surplus-marked-before-close src/fdpass.c \
    'for (size_t index = 0; index < kept; ++index) {
            int descriptor_flags' 'for (size_t index = 0; index < count; ++index) {
            int descriptor_flags'
run_mutant_linux fdpass-receive-without-cmsg-cloexec src/fdpass.c \
    '#define FDPASS_RECEIVE_FLAGS MSG_CMSG_CLOEXEC' '#define FDPASS_RECEIVE_FLAGS 0'
run_mutant_darwin fdpass-control-read-past-copy src/fdpass.c \
    'size_t available = announced < present ? announced : present;' \
    'size_t available = announced + 0u * present;'
run_mutant_linux thread-name-not-truncated src/thread.c \
    '#define THREAD_NAME_LIMIT 15u' '#define THREAD_NAME_LIMIT 63u'
run_mutant_darwin unix-byte-receive-ignores-rights src/socket.c \
    'if (socket_handle->domain == AF_UNIX) {' 'if (0) {'
run_mutant unix-datagram-empty-is-eof src/socket.c \
    'socket_handle->type == SOCK_STREAM, buffer, capacity, out_received);' \
    '1, buffer, capacity, out_received);'
run_mutant_darwin unix-control-only-is-eof src/fdpass.c \
    'if (!control) return MAELYS_SYS_ERR_CLOSED;' \
    'if (1) return MAELYS_SYS_ERR_CLOSED;'
run_mutant_linux condition-wall-clock src/thread.c \
    'status = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);' 'status = 0;'

run_mutant stream-progress-lost src/fdpass.c \
    '*out_sent = (size_t)sent;' '*out_sent = 0;'
run_mutant stream-blocking-socket-admitted src/fdpass.c \
    'flags < 0 || !(flags & O_NONBLOCK) ||' 'flags < 0 ||'
run_mutant stream-empty-rights-admitted src/fdpass.c \
    'passed_fd < -1 || (!length && passed_fd >= 0)' 'passed_fd < -1'
run_mutant stream-eintr-retried src/fdpass.c \
    'send_message(socket_fd, bytes, length, passed_fd, 0)' \
    'send_message(socket_fd, bytes, length, passed_fd, 1)'
run_mutant_linux stream-unexpected-control-hidden src/fdpass.c \
    'flags |= MAELYS_SYS_FDPASS_UNEXPECTED_CONTROL;' 'flags |= 0;'

# Directory watching (0.12): what keeps the two hosts answering alike, and
# what the bookkeeping of entries must not get wrong.
run_mutant dirwatch-gone-keeps-entry src/dirwatch.c \
    'if (entry->pending & MAELYS_SYS_DIRWATCH_GONE) release_entry(dirwatch, entry);' \
    'if (0) release_entry(dirwatch, entry);'
run_mutant dirwatch-pending-not-cleared src/dirwatch.c \
    'else entry->pending = 0;' 'else (void)0;'
run_mutant dirwatch-no-turns src/dirwatch.c \
    'dirwatch->cursor = (last + 1) % dirwatch->capacity;' \
    'dirwatch->cursor = (last + 1) % 1u;'
run_mutant dirwatch-overflow-marks-one-entry src/dirwatch.c \
    'static void mark_overflow(maelys_sys_dirwatch_t *dirwatch) {
    for (size_t index = 0; index < dirwatch->capacity; ++index) {' \
    'static void mark_overflow(maelys_sys_dirwatch_t *dirwatch) {
    for (size_t index = 0; index < 1u; ++index) {'
run_mutant dirwatch-owner-unchecked src/dirwatch.c \
    'return pthread_equal(dirwatch->owner, pthread_self());' \
    'return pthread_equal(dirwatch->owner, dirwatch->owner);'
run_mutant_linux dirwatch-follows-the-writer src/dirwatch.c \
    'while (taken < owed) {' 'while (taken < owed || owed) {'
run_mutant_linux dirwatch-first-cell-taken-for-the-watch src/dirwatch.c \
    'if (entry->wd == wd) return entry;' 'if (entry->wd) return entry;'
run_mutant_linux dirwatch-index-gap-left src/dirwatch.c \
    'if (((at - home) & mask) >= ((at - hole) & mask)) {' \
    'if (((at - home) & mask) > mask) {'
run_mutant_darwin dirwatch-follows-the-writer src/dirwatch.c \
    'size_t batches = dirwatch->capacity / BATCH + 1u;' \
    'size_t batches = SIZE_MAX;'
run_mutant_darwin dirwatch-event-slot-ignored src/dirwatch.c \
    'size_t index = (size_t)(uintptr_t)event->udata;' \
    'size_t index = (size_t)(uintptr_t)event->udata * 0u;'
run_mutant_linux dirwatch-write-in-place-reported src/dirwatch.c \
    '#define DIRWATCH_ENTRIES (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)' \
    '#define DIRWATCH_ENTRIES (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE)'
run_mutant_linux dirwatch-metadata-reported src/dirwatch.c \
    '#define DIRWATCH_ENTRIES (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)' \
    '#define DIRWATCH_ENTRIES (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB)'
run_mutant_linux dirwatch-renamed-directory-kept src/dirwatch.c \
    '#define DIRWATCH_SELF (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)' \
    '#define DIRWATCH_SELF (IN_DELETE_SELF | IN_UNMOUNT)'
run_mutant_linux dirwatch-same-directory-twice src/dirwatch.c \
    'dirwatch->entries[index].wd == wd' '0'
run_mutant_linux dirwatch-link-followed src/dirwatch.c \
    'DIRWATCH_MASK | IN_DONT_FOLLOW | IN_ONLYDIR' 'DIRWATCH_MASK | IN_ONLYDIR'
run_mutant_darwin dirwatch-metadata-reported src/dirwatch.c \
    '#define DIRWATCH_ENTRIES (NOTE_WRITE | NOTE_LINK)' \
    '#define DIRWATCH_ENTRIES (NOTE_WRITE | NOTE_LINK | NOTE_ATTRIB)'
run_mutant_darwin dirwatch-renamed-directory-kept src/dirwatch.c \
    '#define DIRWATCH_SELF (NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE)' \
    '#define DIRWATCH_SELF (NOTE_DELETE | NOTE_REVOKE)'
run_mutant_darwin dirwatch-same-directory-twice src/dirwatch.c \
    'entry->device == status.st_dev && entry->inode == status.st_ino' '0'
run_mutant_darwin dirwatch-link-followed src/dirwatch.c \
    'O_EVTONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC' 'O_EVTONLY | O_DIRECTORY | O_CLOEXEC'

# Processes (0.13): the table staged, everything else closed, a failed exec
# heard, the number reserved, the ladder, a running program not hidden, an
# identity that is more than a number.
run_mutant process-signal-after-reap src/process.c \
    'if (is_reaped(process)) return MAELYS_SYS_ERR_STATE;
    if (kill(' 'if (0) return MAELYS_SYS_ERR_STATE;
    if (kill('
run_mutant process-ladder-stops-at-term src/process.c \
    'for (int rung = 0; rung < 2; ++rung) {' 'for (int rung = 0; rung < 1; ++rung) {'
run_mutant process-release-hides-a-running-program src/process.c \
    'else if (!reaped) result = MAELYS_SYS_ERR_STATE;' 'else if (0) result = MAELYS_SYS_ERR_STATE;'
run_mutant process-identity-is-only-a-number src/process.c \
    '*out_alive = now.birth == identity->birth && now.boot == identity->boot;' \
    '*out_alive = now.boot == identity->boot;'
run_mutant process-exit-fd-open-without-recheck src/process.c \
    '    result = maelys_sys_process_alive(identity, &alive);
    if (result != MAELYS_SYS_OK || !alive) {' \
    '    result = MAELYS_SYS_OK;
    if (result != MAELYS_SYS_OK || !alive) {'
run_mutant_linux process-table-not-staged src/process.c \
    'staged[index] = fcntl(table->source[index], F_DUPFD_CLOEXEC, table->base);' \
    'staged[index] = table->source[index];'
run_mutant_linux process-everything-kept src/process.c \
    'if (!error && mark_all_cloexec(limit) != 0) error = errno;' \
    'if (!error && limit == 0 && mark_all_cloexec(limit) != 0) error = errno;'
run_mutant_linux process-failed-exec-unheard src/process.c \
    '    if (report >= 0) {
        ssize_t written;' '    if (0) {
        ssize_t written;'
run_mutant_darwin process-table-not-staged src/process.c \
    'error = posix_spawn_file_actions_adddup2(&actions, staged[index], table->target[index]);' \
    'error = posix_spawn_file_actions_adddup2(&actions, table->source[index], table->target[index]);'
run_mutant_darwin process-everything-kept src/process.c \
    'short flags = POSIX_SPAWN_CLOEXEC_DEFAULT;' 'short flags = 0;'
run_mutant_darwin process-ended-before-registration-unreadable src/process.c \
    'EV_SET(&change, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);' \
    'EV_SET(&change, 1, EVFILT_USER, 0, 0, 0, NULL);'

printf '%s\n' "mutation check: $killed/$killed killed"
