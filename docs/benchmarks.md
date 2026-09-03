# Reactor benchmark methodology

`make benchmark` always measures Maelys System and writes CSV output. If
`pkg-config` finds libevent or libev, equivalent optional runners are compiled
and appended. Neither library is a build or runtime dependency.

The two workloads are deliberately small:

- one byte written through a socketpair, one readiness dispatch, one read; and
- arming and dispatch of an immediate one-shot timer (framework objects are
  preallocated where that API distinguishes allocation from arming).

Run a fixed workload with:

```sh
BENCH_ITERATIONS=100000 make benchmark
```

The output columns are implementation, workload, iterations, elapsed
nanoseconds and nanoseconds per operation. Record the CPU model, OS, compiler,
power mode and library versions alongside any published result. Use a pinned
machine, run at least five independent samples and report distributions, not
only the fastest sample.

These microbenchmarks measure dispatch overhead. They do not measure fairness,
memory under load, network throughput, tail latency, API safety or application
performance. CI compiles and runs them but has no performance threshold:
shared-runner timing is not a regression oracle. No marketing comparison may
be based on this harness alone.
