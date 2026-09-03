# Changelog

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
