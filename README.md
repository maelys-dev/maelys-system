# Maelys System

`libmaelys-sys` is a deliberately small, standalone POSIX systems foundation
for security-sensitive C programs. It decides low-level semantics once: clocks
and deadlines, descriptor ownership, mechanical POSIX socket lifecycle,
SIGPIPE-safe socket writes, wakeups,
opaque synchronization handles and callback-free readiness dispatch.

Supported platforms for the 0.x series are Linux (amd64/arm64) and macOS on
Apple Silicon. The API is explicitly POSIX and may accept `int` descriptors;
library-owned objects remain opaque and expose no native structure. Windows,
IOCP and WFP are separate product work and are not 0.x goals.

## Constitution

A primitive enters this repository only when:

1. at least two real Maelys consumers need it;
2. it knows no domain type;
3. it adds no external dependency;
4. Linux and macOS expose the same observable semantics;
5. it is testable without MCP, Sandbox or Executor; and
6. adversarial failure-path tests ship with the primitive.

JSON, HTTP, TLS, policy, MIR, process launchers, sandbox backends and business
receipts are intentionally excluded. SHA-256 is shared code, not a system
primitive, and is excluded as well.

`mcp-runtime` remains autonomous. It is the behavior reference for several
deadline, wakeup and SIGPIPE contracts, not a link-time consumer.

## Standalone surface

The library has no mandatory dependency beyond the host POSIX C and threading
runtime. Maelys Egress is a real pinned consumer. Complete generic examples cover
a bounded TCP relay, one-shot timer service and cross-thread wakeup:

```sh
make examples-check
make mutation-check
make benchmark
```

The benchmark always emits a Maelys baseline and adds libevent/libev only when
they are installed. It does not impose a performance threshold or support
marketing claims by itself.

## Build

```sh
make check
make asan-ubsan
make tsan
make install-check
```

The installed surface is split into modular headers under `maelys/sys/` plus
the optional umbrella header `maelys/sys.h`.

## Ownership rules

- `maelys_sys_fd_close(&fd)` invalidates the caller's integer before calling
  `close(2)` and never retries after `EINTR`.
- `maelys_sys_socket_create` and `maelys_sys_socket_accept` return opaque owned
  handles which are already non-blocking, close-on-exec and SIGPIPE-safe.
- Close-on-exec is atomic with creation on Linux; macOS applies it right
  after creation, so a fork+exec racing in another thread can inherit the
  descriptor. Process launchers close descriptors after fork.
- With `MAELYS_SYS_STRICT_RESULTS` defined, functions returning
  `maelys_sys_result_t` are marked `warn_unused_result`; cast to `(void)`
  where ignoring the result is intended. Clang honors the cast, GCC warns
  through it, so the option suits Clang builds.
- Socket connect, bind, listen, accept and I/O functions make one mechanical
  POSIX decision. DNS, retry order, TLS and protocol policy remain consumers.
- `maelys_sys_socket_detach(&handle, &fd)` hands the descriptor to the caller
  and frees the handle without closing; a connection still in progress is
  refused so that completion stays with `connect_complete`.
- Wakeup destruction requires all concurrent signalers to have stopped.
- Mutexes and conditions must not be destroyed while another thread uses them.
- On `MAELYS_SYS_ERR_OS`, `errno` identifies the failed POSIX operation.

## Reactor contract

`maelys_sys_loop` returns readiness events; it never invokes callbacks. Watches
borrow descriptors and carry generation-checked identities. Timers are one-shot
absolute monotonic deadlines. All mutation and `step` calls belong to the
creating thread; only `wake` and idempotent `stop` may cross threads. Every
backend reports the same events: at most one per watch and per step, `HUP` on
peer half-close, and a wake that does not fit in the caller's array stays
pending rather than being lost.

`poll` is the reference backend. Linux selects `epoll`; macOS selects `kqueue`.
The test suite runs every available backend against the same behavior and adds
representative Egress relay and Orchestrator output-capture state machines.

## Documentation

- [Positioning and non-goals](docs/positioning.md)
- [ABI and proposed LTS lifecycle](docs/abi-lifecycle.md)
- [Adoption guide](docs/adoption.md)
- [Examples](docs/examples.md)
- [Benchmark methodology](docs/benchmarks.md)
- [Mutation gate](docs/mutation-testing.md)
- [Release process](RELEASING.md)

## License

MPL-2.0; see [LICENSING.md](LICENSING.md).
