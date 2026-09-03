# Positioning and non-goals

Maelys System is a small, callback-free POSIX foundation for security-sensitive
C programs. It is useful when a program needs explicit ownership, absolute
deadlines, cross-thread wakeups and a readiness reactor, but does not want a
general application framework or an external runtime dependency.

It is not a smaller clone of libuv or libevent. The distinction is intentional:

| Concern | Maelys System | General event framework |
| --- | --- | --- |
| Dispatch | caller-owned event array | callbacks |
| Scope | POSIX primitives | networking/application utilities |
| Runtime dependencies | none | project-dependent |
| Policy/TLS/HTTP/DNS | excluded | often bundled or integrated |
| Threading model | one explicit loop owner | framework-dependent |

The following do not belong in the 0.x product:

- TLS, HTTP, DNS, filesystem watchers or stream abstractions;
- process spawning, child supervision or sandbox policy;
- Windows, IOCP or WFP support;
- JSON, MCP, receipts or other domain types; and
- performance claims based on a single machine or a microbenchmark.

New primitives still have to satisfy the six admission rules in the README.
The narrow scope is a feature: consumers can audit the library without also
accepting a protocol stack.

## Current evidence

Maelys Egress is a real link-time consumer of clocks, descriptors, SIGPIPE-safe
writes and the reactor. The repository also carries generic relay, timer and
cross-thread examples. Examples demonstrate contracts; they are not claims that
another Maelys product has completed its migration.
