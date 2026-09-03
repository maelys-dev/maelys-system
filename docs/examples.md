# Examples

All examples use the public installed surface and are compiled by `make check`.

- `timer-server TICKS INTERVAL_MS` demonstrates absolute, one-shot monotonic
  timers with explicit re-arming.
- `cross-thread-wakeup` demonstrates a worker publishing state under an opaque
  mutex and waking the owner thread without transferring loop ownership.
- `tcp-relay LISTEN_PORT UPSTREAM_IP UPSTREAM_PORT` is a single-connection,
  loopback-only, bidirectional TCP relay. It uses bounded ring buffers,
  readiness-controlled backpressure and half-close propagation. The upstream
  address must be numeric; DNS is deliberately not supplied by the library.

Build and exercise them with:

```sh
make examples
make examples-check
```

The relay is teaching code, not a production proxy. It has no authentication,
policy, multiplexing or service supervision. Maelys Egress is the product-level
consumer for mediated network access.
