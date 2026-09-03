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

## Pinning

During 0.x, consumers should pin an exact release commit and verify both the
ABI constant and the source identity in CI. Maelys Egress's
`system-integration-check` is the reference: it verifies the pin and proves
that the resulting archive has unresolved references to the expected
`maelys_sys_*` symbols.

`mcp-runtime` remains autonomous and is not expected to link this library.
