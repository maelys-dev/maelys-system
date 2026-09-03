# Behavioral provenance

The deadline, wakeup and SIGPIPE contracts were extracted from the behavior of
Maelys MCP Runtime 0.23.x, especially `src/core/common.c`, without introducing
a link-time dependency. Descriptor and pthread wrapper behavior was informed by
the platform layer currently used by Maelys Orchestrator.

This file records behavioral provenance, not a promise of source-level parity.
Maelys System owns its API and tests after extraction.

