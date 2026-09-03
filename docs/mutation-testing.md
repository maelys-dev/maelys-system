# Reactor mutation gate

`make mutation-check` copies the working tree into isolated temporary
directories, applies five one-line faults and requires the existing tests to
reject every mutant:

- generation increment removed;
- stale watch generation accepted;
- timer heap order reversed;
- caller deadline incorrectly preferred over an earlier timer; and
- timer application token discarded.

The sweep is deterministic and has a 30-second process timeout per mutant. A
changed implementation must update anchors and preserve or strengthen the
fault set. Surviving mutants fail the gate; a missing mutation anchor also
fails it. The harness targets load-bearing invariants and is not a substitute
for a general mutation-testing tool.
