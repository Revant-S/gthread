# Benchmark results

Produced by `make bench`. Every number here was measured on the machine described in each section; none are estimates.

## Context switch cost

```
machine:  12th Gen Intel(R) Core(TM) i5-1235U
cores:    12 online
kernel:   Linux 6.8.0-136-generic
glibc:    2.39
compiler: 13.3.0
governor: powersave
measured: 2026-08-16 UTC
build:    benchmarks and the library they link are both -O2; the library is -Og everywhere else
affinity: pinned to CPU 0 with sched_setaffinity
clock_gettime overhead: 46.4 ns per call (subtracted)
1000000 round trips per run, 11 runs
```

One handover is one thread giving the CPU to the other; a round trip is two.

| mechanism | ns/handover (median) | min | max | vs green |
|---|---|---|---|---|
| green | 1460.9 | 1446.3 | 1494.2 | 1.00x |
| condvar | 5065.3 | 4830.3 | 5285.4 | 3.47x |
| pipe | 2444.8 | 2374.7 | 2670.6 | 1.67x |

## Thread creation cost

```
machine:  12th Gen Intel(R) Core(TM) i5-1235U
cores:    12 online
kernel:   Linux 6.8.0-136-generic
glibc:    2.39
compiler: 13.3.0
governor: powersave
measured: 2026-08-16 UTC
build:    benchmarks and the library they link are both -O2; the library is -Og everywhere else
affinity: pinned to CPU 0 with sched_setaffinity
100000 create+join pairs per run, 11 runs
```

| mechanism | us per thread (median) | min | max | vs green |
|---|---|---|---|---|
| green | 2.625 | 2.594 | 2.857 | 1.0x |
| pthread | 18.748 | 18.278 | 19.269 | 7.1x |

## Memory per idle thread

```
machine:  12th Gen Intel(R) Core(TM) i5-1235U
cores:    12 online
kernel:   Linux 6.8.0-136-generic
glibc:    2.39
compiler: 13.3.0
governor: powersave
measured: 2026-08-16 UTC
build:    benchmarks and the library they link are both -O2; the library is -Og everywhere else
affinity: pinned to CPU 0 with sched_setaffinity
10000 idle threads per runtime, each measured in its own forked process
pthread default stack size: 8192 KiB
green stack size: 32 KiB
```

| runtime | threads created | RSS/thread (KiB) | virtual/thread (KiB) | total RSS delta (KiB) |
|---|---|---|---|---|
| green (32 KiB stacks) | 10000 | 4.01 | 0.0 | 40128 |
| pthread (8192 KiB default stacks) | 10000 | 8.33 | 8196.3 | 83296 |

pthread creation did not fail at 10000 threads on this machine.

## Echo server, end to end

```
machine:  12th Gen Intel(R) Core(TM) i5-1235U
cores:    12 online
kernel:   Linux 6.8.0-136-generic
glibc:    2.39
compiler: 13.3.0
governor: powersave
measured: 2026-08-16 UTC
build:    benchmarks and the library they link are both -O2; the library is -Og everywhere else
affinity: pinned to CPU 0 with sched_setaffinity
server pinned to CPU 0, load generator to CPU 1
64 byte request and reply, 11 windows of 1.0s per configuration
```

| server | connections | req/s (median) | min | max | p50 (us) | p99 (us) |
|---|---|---|---|---|---|---|
| green | 1000 | 117234 | 110633 | 124398 | 8327.6 | 39058.2 |
| green | 5000 | 81329 | 76269 | 83400 | 58259.2 | 123178.4 |
| green | 10000 | 72657 | 62929 | 75695 | 123683.7 | 265141.0 |
| pthread | 1000 | 86476 | 73133 | 110855 | 11308.5 | 33394.5 |
| pthread | 5000 | 54228 | 49615 | 57786 | 82923.7 | 126301.2 |
| pthread | 10000 | 52173 | 48616 | 58857 | 165253.4 | 280541.9 |

Both servers are confined to a single core so that the comparison is concurrency against concurrency. The pthread server would scale across cores on a real deployment; this runtime cannot, and that is the honest limitation of N:1.

