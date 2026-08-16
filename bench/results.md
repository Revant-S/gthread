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
clock_gettime overhead: 12.5 ns per call (subtracted)
1000000 round trips per run, 11 runs
```

One handover is one thread giving the CPU to the other; a round trip is two.

| mechanism | ns/handover (median) | min | max | vs green |
|---|---|---|---|---|
| green | 45.8 | 44.6 | 47.1 | 1.00x |
| condvar | 2681.2 | 2650.1 | 2764.3 | 58.57x |
| pipe | 1381.7 | 1337.5 | 1405.7 | 30.18x |

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
| green | 0.332 | 0.331 | 0.348 | 1.0x |
| pthread | 10.451 | 10.165 | 11.377 | 31.5x |

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
| pthread (8192 KiB default stacks) | 10000 | 8.33 | 8196.3 | 83304 |

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
| green | 1000 | 211840 | 145800 | 226488 | 4748.6 | 30225.0 |
| green | 5000 | 153030 | 139191 | 156618 | 31040.2 | 63052.4 |
| green | 10000 | 128386 | 114074 | 134589 | 72690.7 | 103852.4 |
| pthread | 1000 | 148896 | 145859 | 185945 | 6651.4 | 15019.2 |
| pthread | 5000 | 111565 | 107495 | 126449 | 41050.6 | 58698.0 |
| pthread | 10000 | 87464 | 80814 | 101326 | 99373.8 | 158016.9 |

Both servers are confined to a single core so that the comparison is concurrency against concurrency. The pthread server would scale across cores on a real deployment; this runtime cannot, and that is the honest limitation of N:1.

