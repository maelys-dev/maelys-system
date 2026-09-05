# Reactor mutation gate

`make mutation-check` copies the working tree into isolated temporary
directories, applies eighteen one-line faults and requires the existing
tests to reject every mutant. Nine target the reactor:

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

The sweep is deterministic and has a 30-second process timeout per mutant. A
changed implementation must update anchors and preserve or strengthen the
fault set. Surviving mutants fail the gate; a missing mutation anchor also
fails it. The harness targets load-bearing invariants and is not a substitute
for a general mutation-testing tool.
