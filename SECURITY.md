# Security policy

Security fixes are provided for the latest published 0.x minor release.

Do not open a public issue for a suspected vulnerability. Use GitHub private
vulnerability reporting and include the affected version, operating system,
architecture, a minimal reproducer, and the expected impact.

This library normalizes POSIX mechanics. It is not a sandbox, policy decision
point, network firewall, cryptographic provider or process supervisor. Its
security boundary is the documented ownership and concurrency contract of each
primitive.

