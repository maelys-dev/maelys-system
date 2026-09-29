# Reactor mutation gate

`make mutation-check` copies the working tree into isolated temporary
directories, applies forty-two one-line faults, four of them on Linux only
and three on macOS only, and
requires the existing tests to reject every mutant. Nine target the reactor:

- generation increment removed;
- stale watch generation accepted;
- timer heap order reversed;
- caller deadline incorrectly preferred over an earlier timer;
- timer application token discarded;
- compacted timer heap not rebuilt into a heap;
- freed watch slot never returned to the free list;
- cancelled timer not counted as a dead heap node; and
- dead count not reset by a compaction.

The last two are visible only to `tests/test_internals.c`, which compiles
`src/loop.c` into the test to check the heap invariant and the dead-node
count after every operation, against a reference model.

Nine target the file primitives, each a fault a cold review of
`src/file.c` had injected and found the tests of the time unable to see:

- the path not re-resolved to the locked inode after `flock`;
- the expectations not re-applied after `flock`;
- a held lock reported as an OS error instead of `ERR_BUSY`;
- an existing destination reported as an OS error instead of `ERR_EXISTS`;
- `O_NONBLOCK` left on the descriptor after `open(2)`;
- a bounded read that ignores bytes beyond the buffer;
- the final mode applied before the content is written;
- an exclusive file created readable instead of 0600; and
- a conditional removal that ignores the identity it was given.

`tests/test_file_faults.c` compiles `src/file.c` with a fault point before
each system call the contracts speak about; the mode and lock mutants are
caught through actions taken at those points.

Eight more come from the cold audits of 0.8.0 and 0.9.0, which found these
contracts unobserved: EINTR reported by `step` and by `read_bounded`
instead of being resumed; `stop` not sticky; `lock_release` keeping the
lock; the parent sync of a publication aimed at the staging's directory
instead of the destination's, or skipped; and, on Linux only, thread names
not truncated to the host limit and conditions waiting on the wall clock
instead of the monotonic one.

Nine target descriptor passing (0.10): the surplus beyond the caller's
capacity left open; the descriptors left open when setting close-on-exec
fails; a peer gone, and a full queue, reported as an OS error rather than
one code on both hosts; the socket type left unchecked; close-on-exec not
set by the `fcntl` branch, or, on Linux only, not requested from the
kernel; an empty datagram called truncated; and, on macOS only, the
control data read past what the kernel copied.

Two more target byte-only Unix receives on macOS (0.10.1): bypassing control
cleanup and treating a control-only record as EOF. The Linux kernel does not
expose these two faults; both hosts run the descriptor-count regression,
including Linux SO_PASSCRED at EOF.

Five target partial stream operations (0.11): losing the send byte count,
accepting a blocking socket, accepting empty rights-bearing sends, hiding
EINTR by retrying, and hiding unexpected ancillary data (Linux credentials).

The sweep is deterministic and has a 60-second process timeout per mutant. A
changed implementation must update anchors and preserve or strengthen the
fault set. Surviving mutants fail the gate; a missing mutation anchor also
fails it. The harness targets load-bearing invariants and is not a substitute
for a general mutation-testing tool.
