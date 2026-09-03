# ABI and support lifecycle

## 0.x

The installed ABI is identified at runtime by `maelys_sys_abi_version()` and at
compile time by `MAELYS_SYS_ABI_VERSION`. Patch releases preserve source and
binary compatibility. A minor release may add symbols and opaque types without
changing ABI 1.

The 0.5 socket handle is an additive opaque type. Its private state may evolve
without an ABI event; the native descriptor view is borrowed and the existing
ABI 1 number remains correct.

An incompatible public-header or observable-contract change requires all of:

1. an ABI number increment;
2. a major or pre-1.0 minor version event called out in the changelog;
3. a migration document;
4. rebuilds of every pinned Maelys consumer; and
5. parallel install naming if two ABIs must coexist.

Private structure layout, backend implementation and test-only code are not
ABI. Native descriptor integers are part of the explicitly POSIX contract.

## 1.x proposal

ABI 1 is a candidate for the 1.x LTS line only after it has run in at least two
independent real consumers and completed a documented compatibility audit.
Once 1.0 is declared, the project intends to maintain the latest 1.x minor for
24 months after its release and publish security fixes for the entire support
window. This is a policy proposal, not a current 0.x support promise.

Deprecations must remain callable for one minor release and produce no new
runtime behavior. Removal waits for the next ABI event. Static linking means
applications must rebuild to receive fixes; release notes always state that.
