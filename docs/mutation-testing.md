# Reactor mutation gate

`make mutation-check` copies the working tree into isolated temporary
directories, applies ten one-line faults and requires the existing tests to
reject every mutant:

- generation increment removed;
- stale watch generation accepted;
- timer heap order reversed;
- caller deadline incorrectly preferred over an earlier timer;
- timer application token discarded;
- wake consumed while the caller's event array is already full;
- compacted timer heap not rebuilt into a heap;
- freed watch slot never returned to the free list;
- cancelled timer not counted as a dead heap node; and
- dead count not reset by a compaction.

The last two are visible only to `tests/test_internals.c`, which compiles
`src/loop.c` into the test to check the heap invariant and the dead-node
count after every operation, against a reference model.

The sweep is deterministic and has a 30-second process timeout per mutant. A
changed implementation must update anchors and preserve or strengthen the
fault set. Surviving mutants fail the gate; a missing mutation anchor also
fails it. The harness targets load-bearing invariants and is not a substitute
for a general mutation-testing tool.
