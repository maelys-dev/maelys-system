# Adoption guide

Install the static library and metadata with `make install`, then use
`pkg-config --cflags --libs maelys-sys`. Consumers may include the umbrella
header or only the modules they need.

## Reactor migration

1. Keep application state and dispatch in the caller.
2. Create one loop on its permanent owner thread.
3. Register borrowed descriptors with application tokens.
4. Treat watch IDs as generation-checked capabilities, never as descriptors.
5. Unwatch before closing the borrowed descriptor.
6. Use absolute monotonic deadlines and re-arm one-shot timers explicitly.
7. Restrict cross-thread calls to `wake` and idempotent `stop`.
8. Join signalers before destroying the loop.

The loop returns arrays and never calls application code. A callback-based
consumer should therefore translate callbacks into its own state-machine
switch rather than putting callback pointers into Maelys System.

## Process launching

`maelys/sys/process.h` (0.13) replaces a launcher a consumer wrote itself:
one that forks, arranges descriptors, execs, waits with a timeout and
escalates from SIGTERM to SIGKILL. What it replaces, and what it does not:

| Consumer | Replaced by this library | Kept by the consumer |
| --- | --- | --- |
| a protocol launcher (a provider over a socket pair on 0 and 1, or on 3 with the outputs elsewhere) | the launch, the exact descriptor layout as a table, the end as a loop event, the ladder | the protocol, the pipes, the framing, the retry policy |
| a watcher of processes it did not start | the identity (number, birth, boot), `alive`, the end as a descriptor | what to do when a process is gone |
| a program invoker that keeps 0, 1 and 2 as they are and judges the file first | nothing yet; it may call `spawn` under its own checks later, with `{0,0}, {1,1}, {2,2}` in its table | the search of PATH, the shebang, the judgement on the file |

Rules a launcher keeps when it adopts: it never reaps blindly (no
`wait(-1)`), leaves SIGCHLD neither ignored nor `SA_NOCLDWAIT`, unwatches
the exit descriptor before `release`, and sizes the descriptor limit for
one descriptor per program followed. A handler of SIGCHLD that wakes a
loop writes to a pipe of its own, never to a wakeup.

## Pinning

During 0.x, consumers should pin an exact release commit and verify both the
ABI constant and the source identity in CI. Maelys Egress's
`system-integration-check` is the reference: it verifies the pin and proves
that the resulting archive has unresolved references to the expected
`maelys_sys_*` symbols.

`mcp-runtime` remains autonomous and is not expected to link this library.
