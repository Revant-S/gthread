# gthread

A preemptive user-level threading library for Linux, with CSP channels and an
epoll netpoller. Plain C11, no dependencies, one OS thread.

**The thesis:** context switching does not require the kernel, and once you own
the scheduler you can make blocking I/O look synchronous without it being
synchronous.

Many green threads are multiplexed onto a single OS thread. They are preempted
by a periodic signal rather than by the kernel scheduler, synchronised with
semaphores, priority-inheriting mutexes and Go-style channels, and parked on
`epoll` when they touch a socket - so code that reads like blocking I/O never
blocks the process.

## Build and run

```sh
make            # library, tests and demos at -Og with all warnings on
make test       # the acceptance tests, one per milestone
make demo       # CSP pipeline: generator -> filter -> consumer, plus select
make deadlock   # two mutexes taken in opposite orders; hangs on purpose
make server     # TCP echo server, one green thread per connection
make bench      # every benchmark, writing bench/results.md (takes ~8 minutes)
make asan       # the test suite under AddressSanitizer and UBSan
```

The echo server takes a port and a connection limit:

```sh
./build/echo_server 9001 12000
# in another terminal
printf 'hello\n' | nc 127.0.0.1 9001
```

## The concurrency model

Every green thread has a control block and a 64 KiB stack taken from a pool
that `gth_init()` reserves up front, each stack preceded by a `PROT_NONE` guard
page so an overflow is an immediate, diagnosed `SIGSEGV` rather than silent
corruption of a neighbour. Scheduling is fixed-priority preemptive with round
robin inside a level: the scheduler picks the highest non-empty of 32 priority
levels with `31 - __builtin_clz(ready_bitmap)`, which is one instruction and
does not care how many threads exist. A `SIGALRM` timer fires every 10 ms and
the handler switches away from whatever was running, so a thread that never
yields is still preempted. Anything that touches the run queue does so inside a
critical section, which after M7 is a userspace counter rather than a blocked
signal: the handler checks it, and if a section is held it records that a
preemption is owed and returns, leaving the debt to be paid when the section is
released. Blocking I/O is not blocking - `gth_read` and friends set `O_NONBLOCK`,
and on `EAGAIN` they register the fd with `epoll` and park the calling thread
until the scheduler's poll says it is ready.

## The switch path

```
   running thread                  scheduler                   next thread
   --------------                  ---------                   -----------
   gth_chan_recv()
     enter_critical                                             (blocked on
     park on wait list                                           the channel)
     _setjmp(self->jb) --------->  _longjmp arrives here
                                   pick highest priority
                                   wake expired sleepers
                                   epoll_pwait if any I/O
                                   _setjmp(sched_jb)
                                   _longjmp(next->jb) ------->   returns from
                                                                 its own
                                                                 _setjmp
                                                                 exit_critical
```

Switches always go thread -> scheduler -> thread, never thread -> thread. It
costs a second jump and buys one place where the run queue is read, one place
where a dead thread's slot is reclaimed, and one place where the runtime idles;
no blocking primitive contains any scheduling policy.

## Numbers

All measured on this machine, on the dates given, by `make bench`. Nothing here
is an estimate. The full output, including min and max for every figure, is in
[bench/results.md](bench/results.md).

```
machine:  12th Gen Intel(R) Core(TM) i5-1235U, 12 cores online
kernel:   Linux 6.8.0-136-generic
glibc:    2.39     compiler: gcc 13.3.0     governor: powersave
measured: 2026-08-16/17 UTC
```

### What M7 changed

M1 through M6 switched contexts with `swapcontext()` and implemented critical
sections with `sigprocmask()`. Both are correct and both are syscalls, on the
hottest path in the runtime. `strace -c` on the ping-pong benchmark, green
threads only:

| | rt_sigprocmask calls | per handover | total syscalls |
|---|---|---|---|
| before M7 | 7,221,699 | 6.02 | 7,233,592 |
| after M7 | 27 | 0.00002 | 1,129 |

The remaining 27 are thread startup and the once-per-preemption unmasking; the
steady-state loop makes none. The fix was `_setjmp`/`_longjmp` for the switch
(the underscore variants deliberately leave the signal mask alone) and deferred
preemption for critical sections.

Both versions built at `-O2` and run back to back on an otherwise idle machine,
1,000,000 round trips per run, median of 11:

| | before M7 | after M7 | change |
|---|---|---|---|
| green thread handover | 729.6 ns | 53.2 ns | 13.7x faster |
| pthread condvar handover (control) | 2925.9 ns | 3020.6 ns | unchanged, as expected |
| pthread pipe handover (control) | 1482.9 ns | 1475.6 ns | unchanged, as expected |
| create + join a thread | 1.324 us | 0.330 us | 4.0x faster |
| pthread create + join (control) | 10.439 us | 10.994 us | unchanged, as expected |

The pthread rows are there because they are the control: they did not change,
which is what makes the green rows believable.

### Against pthreads

From the same `make bench` run (M7 code):

| mechanism | ns per handover | vs green |
|---|---|---|
| gthread over an unbuffered channel | 45.8 | 1.0x |
| two pthreads over a condition variable | 2681.2 | 58.6x |
| two pthreads over a pipe | 1381.7 | 30.2x |

| | us to create and join | vs green |
|---|---|---|
| green thread | 0.332 | 1.0x |
| pthread | 10.451 | 31.5x |

| 10,000 idle threads | RSS per thread | virtual per thread |
|---|---|---|
| green (32 KiB stacks) | 4.01 KiB | 0 (the pool is reserved up front) |
| pthread (8 MiB default stacks) | 8.33 KiB | 8196 KiB |

Echo server, 64-byte request and reply, server and load generator each pinned to
their own core:

| server | connections | req/s | p50 | p99 |
|---|---|---|---|---|
| green | 1,000 | 211,840 | 4.7 ms | 30.2 ms |
| green | 5,000 | 153,030 | 31.0 ms | 63.1 ms |
| green | 10,000 | 128,386 | 72.7 ms | 103.9 ms |
| pthread | 1,000 | 148,896 | 6.7 ms | 15.0 ms |
| pthread | 5,000 | 111,565 | 41.1 ms | 58.7 ms |
| pthread | 10,000 | 87,464 | 99.4 ms | 158.0 ms |

Two honest caveats on that table. Both servers are confined to one core, because
the green one has no choice; a pthread server on a real deployment would use all
twelve and would win outright, which is exactly what M:N scheduling would buy
and this runtime does not have. And the pthread server's p99 at 1,000
connections is better than the green server's - the kernel's scheduler is fairer
across many runnable threads than a 10 ms round robin is, and that shows up in
the tail rather than the median.

A third caveat on all of it: the CPU governor is `powersave`, and absolute
figures move by up to 2x between a cold machine and one that has been running
benchmarks for five minutes. Everything in a given table came from a single run
so the comparisons within it hold; comparing a number here against a number from
a different session would not be safe.

## What was checked

- `make test` - five acceptance tests, one per milestone, including 5,000
  simultaneous TCP connections through the netpoller. The netpoll test was run
  22 consecutive times without a failure after the last scheduler fix.
- UndefinedBehaviorSanitizer - clean on the whole suite, no reports.
- AddressSanitizer - four of the five tests pass. `t_netpoll`, the one that
  creates 10,000 threads, deadlocks under ASan and only under ASan; ASan itself
  prints `ASan is ignoring requested __asan_handle_no_return ... False positive
  error reports may follow` first, which is the known ASan/`ucontext`
  interaction (google/sanitizers issue 189). `src/sched.c` already tells the
  sanitizer about the stack switches through
  `__sanitizer_start_switch_fiber`/`__sanitizer_finish_switch_fiber`, which is
  what makes the other four work; it is not enough for this one. The same test
  under UBSan alone, and under no sanitizer, passes every time. Reported here
  rather than dropping the target.
- valgrind - not run. It is not installed on this machine and cannot be
  installed without root, so rather than claim a clean valgrind run there is
  simply no valgrind result to report.

## Deviations from the original plan

Two, both deliberate.

**`SIGALRM` does not use `SA_ONSTACK`.** The alternate signal stack is
installed, and the `SIGSEGV` handler that reports stack overflows runs on it -
that handler is the one that genuinely needs it, since it has to run when a
thread's stack is exhausted. The timer handler must not. It does not return
normally when it preempts; it switches away and stays switched out, so its
frame, including the kernel's signal frame that a later `sigreturn` restores
from, has to survive until that thread runs again. An alternate stack is a
single shared buffer and the kernel puts each new frame at the top of it, so
preempting a second thread would land on the first thread's saved frame. The
handler therefore runs on the green thread's own stack, where nothing else can
touch it, and the guard page below covers the case of a signal arriving when
the stack is nearly full. `src/preempt.c` has the full argument.

**The tick handler does not walk the sleeper list.** It counts the tick and
decides whether to switch, and nothing else. Expiring sleepers happens in the
scheduler, half a microsecond later, where the run queue has a single writer and
nothing is half updated. Same behaviour, strictly less code running in signal
context.

## Limitations

- N:1 only. Every green thread shares one OS thread, so this runtime uses one
  core no matter how many cores there are. M:N scheduling across several OS
  threads is the obvious next step and a much larger project; it is not here and
  nothing in this repository pretends otherwise.
- The thread pool is a fixed size, chosen at `gth_init()`. `go_spawn` returns
  `EAGAIN` when it is full rather than growing.
- A green thread parked on an fd cannot be cancelled; `gth_close` on such an fd
  returns `EBUSY`.
- One reader and one writer per fd.
- `errno` is saved and restored across switches, but any other process-global
  state a green thread leaves behind is shared, and the C library's locks are
  not aware of green threads: two green threads in `malloc` at once would
  deadlock on the arena lock, so the runtime does not allocate on any path a
  preempted thread can reach.
- No work stealing, no stack growth, no garbage collection. Deliberately.

## Prior art

`libtask` (Russ Cox), `libmill` and `libdill` (Martin Sustrik) solve the same
problem and were read while thinking about this one; the `_setjmp`/`_longjmp`
switch in particular is their approach and the credit for it is theirs. Go's
runtime is the reference for the netpoller and for what `select` should feel
like. No code was copied from any of them.

## Layout

```
include/    gthread.h  public thread and I/O API
            sync.h     semaphore, mutex, waitgroup, channel, select
            internal.h TCB, run queue, scheduler internals
src/        sched.c    run queue, scheduler, context switching
            preempt.c  timer, SIGALRM handler, critical sections
            stack.c    stack pool with guard pages
            sync.c     semaphore, mutex with priority inheritance
            chan.c     channels, rendezvous, select
            netpoll.c  epoll integration
            gthread.c  public API surface
demo/       pipeline.c deadlock.c echo_server.c
bench/      ctxswitch.c spawn.c scale.c echo_bench.c results.md
test/       one acceptance test per milestone
docs/       EXPLAIN.md - how it works, in answers to the obvious questions
```
