# Changelog

## 0.8.1 - 2026-09-05

- Reactor: a watch id and a timer id can no longer collide (a kind bit in
  the id, 31-bit generations); cancelling a timer with a watch id or
  unwatching with a timer id is `ERR_NOT_FOUND`.
- Reactor: `unwatch` releases the registration of a descriptor closed
  before it (a contract fault the backends answered three different ways)
  and reports OK; loop.h says what a surviving dup does on epoll.
- Reactor: the loop asks the backend for exactly the caller's capacity,
  since the kernel rotates its ready list on what was reported, and the
  poll backend reports from a rotating cursor: a caller array smaller than
  the ready set no longer starves the watches beyond it, on any backend.
  The wakeup rotates in like any descriptor and is consumed only once
  reported, which makes the "wake consumed when full" guard, and its
  mutant, dead code.
- Reactor: the contract of HUP and ERROR is written as the hosts allow
  (HUP promised with READ only, ERROR an indication); kqueue reports ERROR
  on a reset like epoll does. Regular files are named as not watchable.
- `directory_sync` follows a final symbolic link, as the parent sync of a
  publication already did; `sync_parent` now calls it.
- `deadline_after` can no longer return the INFINITE sentinel as a
  deadline; `connect_start` writes `*out_state` on success only; the
  SIGPIPE status of a detached descriptor on Linux is documented.
- Gates: the mutation script builds each mutant without a time limit and
  bounds only the tests, and says whether a kill came from the tests, the
  compiler or a hang; macOS runs the mutants, ASan/UBSan, TSan and the
  analyzer in CI as Linux did; `make release-check` runs every gate
  RELEASING.md requires; `WERROR=` lets a packaged build survive a newer
  compiler's warnings while the checks keep `-Werror`.
- Docs: `thread.h` says the mutex is held across `condition_wait_until`
  and that OK may be a spurious wakeup; `wakeup.h` says the descriptor is
  borrowed; `docs/architecture.md` covers 0.6 to 0.8; `adapter/PACKAGES`
  declares python3 for the macOS runners as `make check` needs it; the
  send-after-close test expects `ERR_CLOSED` alone.
- Adopt maelys-release 0.6.1: the managed files are regenerated, and
  `.github/workflows/ci.yml` calls the socle's `check-product.yml`, which
  reads `adapter/` itself and checks the drift of the managed files from
  the socle `release.yml` pins; the hand-written drift step is gone. The
  product's own matrix (gcc and clang, two macOS, mutants, benchmark,
  packages) stays next to it.

## 0.8.0 - 2026-09-04

- Add `maelys_sys_file_unlink_same` and `maelys_sys_directory_rmdir_same`:
  remove a path only if it still names the identity given (a lease, a
  staged name, a socket path the caller retires), `ERR_IDENTITY` otherwise.
  The contract names the window no POSIX host can close: the check and the
  removal are two calls, and a rename between them is not detected. The
  white-box test proves that sentence true. `write_exclusive` removes its
  own failed file through `unlink_same`. ABI 1 preserved.

## 0.7.0 - 2026-09-04

- Add `maelys/sys/file.h`, the file primitives seven Maelys repositories
  had each written for themselves: trusted-file expectations and identity
  (`verify`, `path_identity`, `identity_same`, `open_trusted`), a bounded
  read that bounds on bytes read, an exclusive durable write (created
  0600, content, `fchmod`, sync), `file_sync` and `directory_sync` with
  `F_FULLFSYNC` first on macOS, no-replace publication of a file or a
  directory by `renameat2(RENAME_NOREPLACE)` / `renamex_np(RENAME_EXCL)`
  with no fallback, and an identity-checked `flock` handle verified before
  and after the lock with the path re-resolved to the locked inode. Three
  result codes appended: `ERR_EXISTS`, `ERR_BUSY`, `ERR_IDENTITY`. ABI 1
  preserved.
- Tests: `tests/test_file.c` through the public API and
  `tests/test_file_faults.c`, which compiles `src/file.c` with a fault
  point before each system call the contracts speak about and checks
  every failure path's promise.

## 0.6.0 - 2026-09-04

- Watch registration is O(1): a descriptor-indexed table rejects duplicates
  and a free list hands out slots, instead of two linear scans per call.
- The timer heap is compacted once cancelled nodes outnumber live ones, so a
  timeout re-armed on every packet no longer grows the heap with the traffic.
- `MAELYS_SYS_LOOP_AUTO` falls back to poll on a host without epoll or
  kqueue; `backend_available` and `loop_create` share one backend table.
- Functions returning `maelys_sys_result_t` carry `MAELYS_SYS_NODISCARD`,
  which expands to `warn_unused_result` when the consumer defines
  `MAELYS_SYS_STRICT_RESULTS` (off by default; Clang honors a `(void)`
  cast, GCC warns through it). The library's own Clang builds enable it.
- Thread names are truncated to what the host applies (15 bytes on Linux);
  longer names were silently not applied.
- macOS: `_DARWIN_C_SOURCE` keeps `pthread_cond_timedwait_relative_np`
  declared by the SDK even if a feature macro such as `_POSIX_C_SOURCE` is
  ever defined, instead of a local `extern` declaration.
- Document that close-on-exec is atomic with creation on Linux only.
- Build: header dependency tracking (`-MMD`), `-fPIC`, `make analyze` fails
  on findings, the version test reads the `VERSION` file through the
  Makefile instead of a literal, CI actions pinned by commit.
- Tests: a timer compaction order test, a watch slot reuse check, and a
  white-box `tests/test_internals.c` that checks the timer heap invariant
  and the dead-node count after every operation of a random workload
  against a reference model; four reactor mutants added (10 in all).

## 0.5.6 - 2026-09-04

- Add `maelys_sys_socket_detach`: the handle gives its descriptor to the
  caller and is freed without closing, so an integrator can own a socket
  System created and connected. The descriptor keeps close-on-exec and
  non-blocking mode; a connection in progress is refused with `ERR_STATE`.
  ABI 1 is preserved.
- Adopt maelys-release 0.5.0: `adapter/PACKAGES` declares `python3` for
  the Linux runners, the release workflow is regenerated by
  `bin/maelys-release adopt`, and the CI drift step runs
  `bin/maelys-release check . --product maelys-system`.

## 0.5.5 - 2026-09-03

- A cross-thread wake is never lost: when the caller's event array is already
  full, `maelys_sys_loop_step` leaves the wakeup pending for the next step
  instead of consuming it silently (epoll and kqueue ordered the wake after
  descriptor events, poll happened to order it first).
- A watch yields at most one event per step on every backend; kqueue reported
  one event per direction for a READ|WRITE watch.
- The poll backend requests `POLLRDHUP` on Linux and reports a peer half-close
  as `HUP`, as epoll and kqueue already did.
- `maelys_sys_socket_connect_start` no longer reports `EAGAIN` as in progress:
  on Linux AF_UNIX with a full backlog nothing was started, the call now
  fails with `ERR_OS` and the handle stays reusable. `connect_complete`
  confirms a peer exists and returns `ERR_STATE` when called before
  readiness instead of a false success.
- Add `maelys_sys_socket_bind_with` and `maelys_sys_socket_bind_options_t`
  (`reuse_address` sets `SO_REUSEADDR` before bind). `maelys_sys_socket_bind`
  is unchanged and equals NULL options. ABI 1 is preserved.
- The `tcp-relay` example uses the socket handle API end to end, including
  `bind_with`, instead of native calls.
- Add a backend parity test (`tests/test_backends.c`) that runs peer
  half-close, merged directions and wake-with-full-array on poll and on the
  native backend, and a sixth reactor mutant for the wake path.
- Regenerate the release workflow with maelys-release 0.2.8 (the tap publish
  job no longer trips on a duplicate formula class).

## 0.5.4 - 2026-09-03

- The public repository restarts its history at the 0.5.1 tree, the first
  MPL-2.0 release; the MIT-licensed history up to 0.5.0 lives in the private
  repository maelys-dev/maelys-system-archive. The v0.5.3 release and its
  bottles were published by that archived repository: this release replaces
  them, and Egress re-pins to it.
- Regenerate the release workflow with maelys-release 0.2.6: the shared tap
  is tapped before bottles are built, and `workflow_dispatch` with a `tag`
  input replays the Homebrew publication of an existing tag.
- Regenerate the release workflow with maelys-release 0.2.5 (staging tap
  trusted before the bottle digests are merged; `SHA256SUMS` lists only the
  archives that exist).

## 0.5.3 - 2026-09-03

- Regenerate the release workflow with maelys-release 0.2.3, which grants
  the tap job the permissions its bottle attestation needs. The `v0.5.2`
  tag exists but produced no release either: its tap job was refused at
  startup for that reason.

## 0.5.2 - 2026-09-03

- Regenerate the release workflow with maelys-release 0.2.2, whose caller
  declares the permission ceiling GitHub requires for reusable workflows.
  The `v0.5.1` tag exists but produced no release: its workflow failed at
  startup on that rule; this is the first release published through the
  shared socle.

## 0.5.1 - 2026-09-03

- Relicense the repository from MIT to MPL-2.0 (`LICENSE`, `LICENSING.md`,
  package metadata). ABI 1 and every public contract are unchanged.
- Release through the shared maelys-release workflows and publish the
  Homebrew formula `libmaelys-sys`, named after the archive it installs,
  rendered from `packaging/homebrew/libmaelys-sys.rb.in` at the released
  tag; `scripts/package-release.sh` accepts the target name.

## 0.5.0 - 2026-08-31

- Add an opaque POSIX socket handle with non-blocking, close-on-exec and
  SIGPIPE-safe creation and accept paths.
- Add mechanical connect start/completion, partial receive/send, idempotent
  shutdown, bind and listen operations without DNS or application policy.
- Preserve ABI 1: this release only adds opaque types and symbols.

## 0.4.0 - 2026-08-23

- Add standalone positioning, adoption, ABI/LTS and non-goal documentation.
- Add complete TCP relay, timer and cross-thread wakeup examples with smoke and
  end-to-end tests.
- Add reproducible reactor microbenchmarks with optional libevent/libev runners
  and an internal baseline that requires no benchmark dependency.
- Add a deterministic five-mutant reactor gate covering generation identity,
  timer ordering/deadlines and token delivery.
- Add native tarball, Debian, RPM and Homebrew packaging paths plus expanded
  multi-architecture CI/release gates.

## 0.3.1 - 2026-08-23

- Preserve per-call SIGPIPE suppression with `MSG_NOSIGNAL` where available,
  keeping `SO_NOSIGPIPE` only as a documented portability fallback.
- Reject blocking sockets in deadline-bounded complete sends so finite
  deadlines cannot silently be exceeded.
- Clarify the accepted and rejected uses of the infinite-deadline sentinel.
- Add adversarial socket-deadline and reactor timer-ordering coverage informed
  by a targeted mutation sweep.

## 0.3.0 - 2026-08-23

- Prove the reactor with Netd-like relay and Orchestrator-like capture fixtures.
- Add resource-exhaustion, stale-identity, timer and concurrent-wakeup tests.
- Add deterministic high-cardinality reactor stress coverage.
- Add Linux amd64/arm64 and macOS Apple Silicon release CI.
- Add sanitizer, static-analysis, boundary-audit and install/package gates.

## 0.2.0 - 2026-08-23

- Add a callback-free, owner-thread readiness reactor.
- Add generation-checked watch and timer identities.
- Add absolute one-shot timers, cross-thread wake and idempotent stop.
- Add poll, epoll and kqueue backends behind one observable contract.

## 0.1.0 - 2026-08-23

- Add monotonic and wall clocks with overflow-checked absolute deadlines.
- Add descriptor flags, close-on-exec pipes and socketpairs, and safe close.
- Add socket writes that suppress SIGPIPE and honor absolute deadlines.
- Add coalescing cross-thread wakeups.
- Add opaque pthread-backed mutex, condition and thread handles.
- Establish the six-rule admission constitution and explicit POSIX 0.x scope.
