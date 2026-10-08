# Changelog

## 0.13.0 - 2026-10-08

- Processes, `maelys/sys/process.h`: start a program with exactly the
  descriptors named, follow it, stop it within a bound, and tell whether a
  number still names the process it did. Two consumers carry it by hand
  today: a protocol launcher that forks, lays out descriptors, execs,
  waits with a timeout and escalates from SIGTERM to SIGKILL; and a watcher
  of processes it did not start, which needs a number, a birth and a boot
  to be sure of one. A third invoker keeps its own rules (PATH, shebang, a
  judgement on the file, 0, 1 and 2 as they are) and may come under this
  later. The contract was written from measurements on Linux 6.10 and
  macOS 26.6:
  - the table is applied as a whole, which neither host does by itself:
    `posix_spawn` applies its file actions in order and `dup2` in sequence
    does the same, so {3->4, 4->3} copied the old 3 twice on both. Every
    source is first duplicated close-on-exec above every target, and the
    targets are made from the duplicates; the fixed array that holds them,
    with nothing allocated, is what bounds the table at 32;
  - on Linux, "everything else closed" is `close_range` with
    `CLOSE_RANGE_CLOEXEC` over every descriptor, 0 included, before the
    targets are made: marking after making them marks the targets too, and
    `dup2(3, 3)` is a no-op that clears nothing, both measured. A kernel
    without `close_range` (before 5.11) gets the names of `/proc/self/fd`
    read with `getdents64` into a buffer on the stack, in the child, with
    nothing allocated; a system without `/proc` gets every number up to the
    limit. On macOS it is `posix_spawn` with `POSIX_SPAWN_CLOEXEC_DEFAULT`,
    and no fork at all;
  - the end of a program is a descriptor for a loop to watch: a pidfd on
    Linux, a kqueue with `EVFILT_PROC` on macOS. Registering that kqueue on
    a program that has already ended answers `ESRCH`, measured, so the
    descriptor is then made readable by a user event instead, and a wait
    never sleeps for a program that is gone. `pidfd_open` on such a program
    succeeds and is readable at once;
  - a wait is bounded by an absolute monotonic deadline like every wait
    here, sleeps on that descriptor and polls nothing; `terminate` is
    bounded by its two durations; `release` never blocks and says `STATE`
    when it abandons a running program;
  - a missing path is `ERR_NOT_FOUND`; a file the caller may not run is
    `ERR_OS` with `EACCES`, like every refusal that is not the frequent
    one. On macOS the process a refused `posix_spawn` aborted stays
    visible to `waitpid(-1, WNOHANG)` for 15 to 40 microseconds and leaves
    no zombie, measured, and the header says so;
  - an identity is three numbers, compared and never interpreted: the
    number, the birth (`starttime` of `/proc/PID/stat` on Linux, read after
    the last parenthesis; `pbi_start` on macOS) and the boot. A program
    that has ended and is not reaped keeps an identity on Linux and has
    none on macOS, measured; `alive` answers 0 for a namesake and `ERR_OS`
    when the host refuses to say, which a `bool` could not;
  - `fork` costs 0.4 ms and glibc's `posix_spawn` 0.2 ms for a `/bin/true`
    under a parent with 512 MB touched: Linux keeps `fork`, whose child
    runs async-signal-safe calls only.
  Two sides to the reserved number: the caller reaps nothing blindly and
  leaves SIGCHLD neither ignored nor `SA_NOCLDWAIT`; `wake` and `stop` of
  a loop are still never called from a handler of it. `examples/
  process-launch.c` starts a program with three descriptors, relays its
  output from a loop and stops it with the ladder; the installed archive
  and the Homebrew formula start a program and reap it.
- Mutation gate at seventy-nine, eleven of them on processes.

Decisions: the primitive starts exactly what it is told; judging whether a
file may be run stays with the invoker that judges; no wait on a process
is unbounded; no process is ever reaped by -1.

## 0.12.3 - 2026-10-07

One correction to the loop, then every public header read against three
questions: which thread may call, whether a signal handler may, and what is
already done when an error comes back. No function, structure or result
code changes.

- `maelys_sys_loop_step` no longer lets a due timer keep descriptors
  unheard. A step that found a timer due returned it without asking the
  kernel anything, so a timer due again at every step starved every watch.
  Measured on both hosts over 200 steps, with one readable pipe watched: a
  timer re-armed "due at once" was served 200 times and the pipe reported
  never; a 1 ms periodic timer under a consumer that takes 3 ms per turn,
  199 times against once. Every tag of this repository, from 0.5.4, carries
  it. A step now always asks the kernel, without waiting when a timer is due, and spends no timer
  before the kernel has answered. When timers and descriptors could each
  fill the array, they go first in turn. In both measurements the pipe is
  now reported at all 200 steps.
  What a caller may notice: within one array, timers no longer come before
  descriptors, which the contract never promised and now says; and a step
  with a due timer costs one system call it did not make.
- The whole turn is tested, not one call: a loop that watches a directory
  watch, a pipe that stays readable and a timer due again at every turn,
  while 128 directories change before each read of the kernel. Each of
  twelve turns serves all three.
- `examples/directory-watch.c` shows the three rules of `dirwatch.h` in a
  loop: add before the first read, one poll per readiness with an array of
  `entry_capacity` elements, "reread" as the whole message.
- Mutation gate at sixty-eight: four faults on the fairness of a step.

The headers:

- `loop.h`: `wake` and `stop` are never called from a signal handler. They
  go through the loop's wakeup, which takes a mutex on macOS, exactly as
  0.12.2 said of `wakeup.h`; stopping a loop on a signal is the first place
  a caller would try it. No Maelys consumer does. The header also says what
  it left to the code: an owner-thread call from another thread is
  `ERR_STATE`, `destroy` waits for no thread inside `wake` or `stop`, and an
  `ERR_OS` from `step` reports no event and loses none.
- A loop and a directory watch do not cross `fork`, and the hosts differ in
  how it fails. Measured: on Linux the child shares the parent's epoll and
  inotify instances, so an `unwatch` in the child leaves the parent's loop
  silent on a readable descriptor, and a `remove` in the child has the
  parent told `GONE` for a directory that has not moved. On macOS the child
  does not inherit the kqueue and the parent is untouched. Both are tested
  on both hosts.
- `clock.h` says which clocks are read and where the hosts part: on macOS
  the monotonic clock goes on while the machine sleeps (86260 s of sleep
  counted on the host that measured it); Linux documents that it leaves
  suspend out, which nothing here observes. A deadline is not the same
  bound on the two.
- `thread.h` had no contract. A mutex is the default kind: a thread that
  locks one it already holds waits for ever, measured on both hosts. A
  thread is joined exactly once and never detached. The destruction rules
  the README carried are in the header.
- `fd.h`: when `maelys_sys_socket_send_all_until` fails, a deadline
  included, part of the buffer may be sent and the call does not say how
  much. The stream has lost its place; the header now says to close it.
- `socket.h`: "owner-thread-confined" was a rule nothing checks, unlike the
  loop. The header says so.

## 0.12.2 - 2026-10-07

Contracts corrected after a consumer read 0.12.1. No function, structure or
result code changes, and no behaviour: the library does what it did.

- `dirwatch.h` told the caller to poll again while a call fills its array.
  That has no end of its own either: while enough directories keep changing,
  every call fills the array. On Linux 6.10, eight directories written
  without pause and a caller that rereads each changed directory for 200
  microseconds: 100 full calls in a row with an array of four, 306 with an
  array of eight, before chance let it go. The rule is now the one the code
  already kept: give poll an array of `entry_capacity` elements and call it
  once each time the descriptor is readable. An entry is reported at most
  once per call, so nothing stays pending in the handle and the descriptor
  answers for the rest. A caller with a smaller array counts its calls and
  arranges its own return, since the descriptor does not answer for what the
  handle still holds. Both halves are now tested: nothing owed after one
  call with an array of the capacity; thirty-two full calls in a row with a
  smaller one, then a quiet descriptor while changes are still owed.
- What directory watching does not replace, said where 0.12.0 named cx: a
  write in place is reported on neither host, so a caller that reads the end
  of files that grow in place keeps its own reads of their size and date.
  Directory watching tells it that such a file appeared, was removed or was
  replaced by rename. On macOS each entry holds one descriptor: a caller
  that watches more than about two hundred directories raises its limit.
- `wakeup.h` says what a wakeup is for: from one thread to the loop of
  another, never from a signal handler. On macOS signal and consume take a
  mutex, and a handler that interrupts either waits for a lock that will not
  be released. A handler writes one byte to a non-blocking pipe of its own.
- `file.h`: after a publish with `sync_parent`, `ERR_OS` alone does not say
  whether the rename or the sync failed. The staging does, and the contract
  now says so; it also names the two-call form, publish then
  `directory_sync`, for a caller that wants each step to answer for itself,
  with what that form gives up: the parent is reopened by its path.
- `docs/positioning.md` still excluded filesystem watchers from 0.x. It now
  excludes what 0.12 does not do: recursion, file names in events, the
  content and metadata of files.

## 0.12.1 - 2026-10-06

- `maelys_sys_dirwatch_poll` no longer follows a writer. 0.12.0 emptied the
  kernel before it returned: until the inotify queue was empty on Linux, for
  as long as batches of 64 came back full on macOS. Whoever may write in a
  watched directory could so hold the owner thread, often a loop's, inside
  one call. A call now takes what the kernel held when it began, the size of
  the inotify queue read once on Linux, one batch per 64 entries and one
  more on macOS, and leaves the rest queued, where it keeps the descriptor
  readable. Nothing is lost and nothing is reported differently.
- The entry of a kernel event is found without walking the table: the slot
  comes back with the event on macOS, and an index by watch number gives it
  on Linux, for a watch already released as for a live one. 0.12.0 walked
  every entry for every event.
  Measured on Linux 6.10 with writers that never stop, the longest of
  twenty calls: with 16384 entries and ten writers in one directory, 0.12.0
  did not return within 8 s and 0.12.1 takes 2 ms; with the ten writers in
  ten directories, emptying only what was queued still took 0.4 s while
  every event walked the table, and takes 3 ms.
- The contract no longer tells the caller to poll until `ERR_WOULD_BLOCK`,
  which rebuilds the same wait one level up. Poll again only while a call
  fills its array: a call that returns fewer changes than its capacity has
  delivered all the handle held, and the descriptor answers for the rest.
  A caller written to the 0.12.0 sentence stays correct and loses nothing;
  it stays in its own loop for as long as a directory keeps changing.
- The installed archive and the Homebrew formula now watch a directory and
  read one change, where they only created a loop: the component added last
  is exercised through what a consumer receives.
- Mutation gate at sixty-four, nineteen of them on directory watching.

## 0.12.0 - 2026-10-06

- Directory watching, `maelys/sys/dirwatch.h`: "the entries of this
  directory changed: reread it." No file name, no recursion, no file
  content. Two consumers carry it by hand today: codexmanager waits for a
  socket to appear with inotify and with kqueue in two files, and cx watches
  the directories of another program through a daemon it should not need.
  The contract was written from measurements on Linux 6.10 and macOS 26.6:
  - a creation, a removal, a rename and a replacement by rename onto an
    existing name are reported on both hosts, coalesced to one change per
    entry between two polls;
  - a write in place and a change of metadata are reported on neither.
    inotify could report them and is not asked to, so that a caller written
    on Linux does not lean on a signal kqueue never gives;
  - the entry follows the directory, not its path: renaming the directory
    itself is GONE, renaming an ancestor says nothing on either host, and
    the contract names that the absence of GONE proves nothing about the
    path;
  - inotify answers by inode and hands back the watch it already has for a
    directory added twice, so one directory is one entry on both hosts,
    `ERR_EXISTS` otherwise, by whatever path;
  - add before reading the directory, and poll until `ERR_WOULD_BLOCK`: the
    descriptor speaks for the kernel's queue, not for what the handle still
    owes.
  The descriptor goes in a loop on every backend, which `loop.h` now says. A
  kernel limit is `ERR_OS` with its `errno`; on macOS each entry holds one
  open descriptor, against a default limit of 256.
- Mutation gate at fifty-nine, fourteen of them on directory watching.
- Adopt maelys-release 0.62.3: the formula's test runs on a poured bottle at
  release, and a failure keeps the formula out of the tap.
- After a receive, the surplus is closed before anything is marked
  close-on-exec, and only the descriptors returned are marked. On macOS,
  where marking is two `fcntl` calls, a peer attaching 254 descriptors to
  every message cost the receiver three system calls per descriptor; it now
  costs one. A byte-only `maelys_sys_socket_receive` on a Unix socket, where
  every descriptor is surplus, can no longer fail after it has read.
- The stream receive contract says outright what an error after the read
  costs: the bytes are consumed although `out_received` is 0, so the caller
  abandons the exchange instead of retrying.
- A review of 0.11.0 found two faults the tests let through, both now in the
  mutation gate with a third: closing only the descriptors not yet marked
  when marking one fails, ignoring a failed `F_SETFD`, and marking the
  surplus. The white-box test fails marking on the second and third
  descriptor of a batch and on `F_SETFD`, and checks nothing stays open.

## 0.11.0 - 2026-09-29

- Add partial `maelys_sys_fd_stream_send` and `maelys_sys_fd_stream_receive`
  operations in the existing standalone `fdpass.o`, with no ABI bump. They
  require an already-nonblocking Unix stream socket, report byte progress and
  leave framing, deadlines and retries to the consumer. Positive send progress
  queues the descriptor once; a retry of the remainder must not attach it
  again. Empty rights-bearing sends are refused. Receives close surplus
  descriptors, preserve control-only delivery, set CLOEXEC, and flag unexpected
  ancillary data. EINTR is exposed with zero progress rather than hiding an
  unbounded retry inside an operation.
- Measure macOS near-full queues, not only full ones: a 16-byte payload with
  rights and 16--31 freed bytes can block in blocking mode, while nonblocking
  mode reports EAGAIN; with less control space macOS reports EMSGSIZE. The
  stream API treats that rights-specific pressure as WOULD_BLOCK, not other
  uses of EMSGSIZE. Linux SO_PASSCRED can attach credentials to EOF; zero
  bytes without rights still ends the stream.
- Standalone tests cover fragmented and adjacent transfers, partial sends,
  full and near-full queues, zero-byte rights, maximum descriptor surplus,
  credentials, EOF, SIGPIPE, interrupts and fcntl-failure cleanup. Five new
  mutants hold the additive stream contract; datagram semantics are unchanged.

## 0.10.1 - 2026-09-29

- Security: byte-only AF_UNIX socket receives now consume control data and
  close attached descriptors. macOS installs SCM_RIGHTS even when recv(2)
  ignores them; an untrusted local peer could exhaust a server's descriptors.
  Created and accepted sockets share fdpass's cleanup, including short
  reads, adjacent rights-bearing writes and Linux SO_PASSCRED. Control-only
  records are not EOF; draining them is bounded to protect the event loop.
  No public function or ABI number changes. Statically linked consumers
  must rebuild to receive the fix.
- The archive member `fdpass.o` is part of the standalone contract, not only
  the source file. maelys-egress extracts that member from an installed
  `libmaelys_sys.a`, where no source is shipped, into the client archive a
  confined process links. Its name, its being exactly one member and its
  autonomy are now written into ABI 1, and `make check` and `make
  install-check` read it from the built and from the installed archive:
  exactly one member of that name, no undefined symbol of the library or of
  the thread runtime, and a round trip linked against it alone. The check
  used to read the object the build leaves beside the archive, which a
  change of build could have made differ from what ships.

## 0.10.0 - 2026-09-28

- Descriptor passing over AF_UNIX SOCK_DGRAM: `maelys_sys_fd_send` sends one
  datagram and at most one descriptor, `maelys_sys_fd_receive` receives one
  datagram and every descriptor with it, in `maelys/sys/fdpass.h`. Asked by
  maelys-egress, whose channel server and Warden's fd-4 broker and network
  client each carried the same `sendmsg`/`recvmsg` by hand, and already
  handled truncation differently. Measured on both hosts before the
  contract was written:
  - a peer gone or no longer reading is `ERR_CLOSED` on both, where Linux
    says ECONNREFUSED or EPIPE and macOS ECONNRESET or EINVAL; a full peer
    queue is `ERR_WOULD_BLOCK` on both, where Linux says EAGAIN and macOS
    ENOBUFS, and the contract names that macOS never waits even on a
    blocking socket;
  - the receive buffer holds every descriptor a message can carry, 253 on
    Linux and 254 on macOS: with a smaller one macOS installs the
    descriptors anyway, where the caller cannot see them, and may announce
    in `cmsg_len` more than it copied. The surplus beyond the caller's
    capacity is closed here and flagged `SURPLUS`; truncated data and
    control are flagged, never swallowed; each descriptor returned is
    close-on-exec, and the contract names the window macOS leaves between
    `recvmsg` and `fcntl`;
  - macOS flags an empty datagram read into an empty vector as truncated; a
    zero-capacity receive reads into one scratch byte instead.
- `src/fdpass.c` stands alone, and that is part of ABI 1: it names no other
  symbol of the library and needs no thread runtime, so a client linked into
  a confined process compiles it from its pinned checkout without linking
  the library. Its tests link the one object without the archive and
  without `-pthread`, and `make check` refuses any undefined library or
  thread symbol in it.
- Mutation gate at thirty-five, nine of them on descriptor passing, one of
  which only the macOS kernel makes observable.
- The Linux CI legs no longer fail on a broken third-party apt source of the
  runner image: `apt-get update` warns and the install that follows decides.
- A pull request builds each instrumented tree once. The socle's shared
  check already runs `make asan-ubsan` on Linux with clang, and this
  repository ran it again in its own `sanitizers` job. That job keeps its
  name and its two remaining steps, TSan and the static analyzer, which the
  shared job does not cover; the macOS gates keep their own sanitizers,
  which no Linux job can run.
- Adopt maelys-release 0.62.1 (from 0.14.2). The shared check's legs are
  named after what they check, `check (linux)`, `check (linux-arm64)` and
  `check (macos)`, so an image upgrade never renames a check that the
  protection of `main` requires. That protection moved to the new names with
  every other setting kept, and the former names, served as aliases during
  the move, are gone: three jobs fewer per pull request. A release replay
  replays the release as well as the Homebrew publication, and the managed
  instruction blocks carry their CC-BY-4.0 attribution and no longer name a
  documentation repository. This repository's own CI runs once per pull
  request: `push` names `main`, so a branch push no longer duplicates the
  run.

## 0.9.1 - 2026-09-06

- From a cold audit of 0.9.0 and of an oci blind audit's notes on System.
- Publication anchors both parent directories by descriptor, followed
  through a final link and opened before the rename: renameat2 and
  renameatx_np run through them, as does the parent sync, so a replacement
  of either path after the open redirects nothing. The type check and the
  rename stay two calls and the contract names that window; there are two
  states after the call, a crash included. `""` and `"/"` are ERR_ARGUMENT.
  A retry after a failed parent sync is ERR_NOT_FOUND, not ERR_EXISTS as
  the contract said.
- `write_exclusive` puts the inode back to 0600 before removing its failed
  file with unlink_same; a failed initial fstat leaves the empty 0600 file
  rather than unlink a path blind. A retry on the same path stays a fresh
  creation.
- `open_trusted` and `lock_acquire` open with O_NOCTTY: a terminal planted
  at the path was acquired as controlling terminal before being refused.
- A peer's reset seen from the sending side is ERR_RESET on both hosts, as
  0.9.0 promised: macOS reports it as HUP without WRITE and answers EPIPE
  to send, so SO_ERROR is read where Linux says ECONNRESET. Once reported,
  the socket is ERR_CLOSED.
- `accept` maps ECONNABORTED to ERR_WOULD_BLOCK: the pending connection
  was aborted by its peer, the listener is fine. `fd_wait` no longer
  reports a timeout after a slice of INT_MAX ms with the deadline ahead.
- Checkout headers precede consumer `CPPFLAGS` in every rule, and `make
  check` proves it by compiling against a poisoned include directory: an
  installed copy of an older release could shadow the sources under test.
- The timer model test draws past deadlines within the age of the
  monotonic clock instead of failing on a host younger than 16 s.
- Contracts written down: a step reporting STOPPED holds back its batch; a
  dup left alive after unwatch on Linux makes a step spin until its
  deadline. Mutation gate at 27, with the parent sync aimed at the wrong
  directory or skipped both observed.

## 0.9.0 - 2026-09-05

- Two result codes appended, ABI 1 preserved, from the review by
  maelys-http: `ERR_RESET` for a peer's reset (`ECONNRESET`) on receive and
  send, which was reported as `ERR_CLOSED` and let a body delimited by the
  close pass as complete; `ERR_WOULD_BLOCK` for the normal state of a
  non-blocking socket on receive, send, accept and a Unix connect that could
  not start, which was `ERR_OS` with `EAGAIN`. Consumers that mapped
  `ERR_CLOSED` to end of stream or tested `errno == EAGAIN` change behavior:
  that is the point.
- `maelys_sys_fd_wait`: one `poll(2)` on one descriptor with an absolute
  deadline, for a consumer that needs no loop; `send_all_until` uses it.
- `maelys_sys_condition_wait`: a wait without deadline, for a worker with
  nothing to do until told.
- The wakeup is an `eventfd` on Linux: one descriptor, no lock; the pipe
  stays on macOS.
- SIGPIPE is suppressed per call with `MSG_NOSIGNAL` on both hosts; the
  per-call `SO_NOSIGPIPE` fallback was dead code and its comment misleading.
- Rules written down: public structures the library reads keep their fields
  within ABI 1 (a new option is a new structure and function); consumers
  check `MAELYS_SYS_ABI_VERSION` and `maelys_sys_abi_version()`, not the
  version string; the public history is never rewritten again, the restart
  of 2026-09-03 having invalidated every earlier commit pin.

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
