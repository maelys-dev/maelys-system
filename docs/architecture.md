# Architecture

Maelys System sits below domain libraries and exports only dependency-free
POSIX mechanics. The 0.1 line contains clocks, descriptors, socket writes,
wakeups and synchronization. The 0.2 line adds a callback-free readiness
reactor. The 0.3 line hardens both with stress and fault-path coverage. The
0.5 line adds an opaque socket lifecycle used by multiple protocol libraries
without absorbing their DNS, address-selection, TLS or retry policy.

The reactor owns registrations and timer bookkeeping. It never
owns watched descriptors and never runs domain callbacks. Egress and Orchestrator
retain their state machines.

All backends expose level-triggered readiness. Registration, modification,
timers, step and destruction belong to the creating thread; wake and stop are
the only cross-thread operations. Watch and timer IDs include generations so a
reused descriptor or slot cannot receive an event from an earlier lifetime.

The library has three release layers:

- 0.1 establishes clocks, descriptors, wakeups and opaque synchronization;
- 0.2 establishes the public reactor contract and native backends; and
- 0.3 freezes ABI 1 after adversarial, consumer and multi-architecture gates.

No 0.x release contains a stream abstraction. The socket handle is a thin
owned POSIX resource, not a buffered connection, resolver or client. TLS
integrations remain consumer state machines driven by read/write readiness,
including retry direction changes such as a TLS read requesting socket write
readiness.
