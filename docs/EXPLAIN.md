# How gthread works

Answers to the questions this project invites, each pointing at the code that
backs it up. Where the code does not do something, that is said rather than
glossed over.

## 1. What exactly happens on a context switch here?

`gth_switch_to_sched()` in `src/sched.c`. The outgoing thread saves its own
callee-saved registers, stack pointer and program counter into a `jmp_buf`
living in its control block with `_setjmp`, then `_longjmp`s into the
scheduler's `jmp_buf`. Nothing else is saved, because by the x86-64 calling
convention everything else - the caller-saved registers, the vector registers -
is already dead across a function call, and a voluntary switch is a function
call. Two pieces of state that are not registers travel too: `errno`, which is
one variable per OS thread and therefore shared by every green thread, saved
into the TCB by hand; and the thread's position in whatever wait list will wake
it, which it joined before switching. Nobody but the thread itself does the
saving. The kernel is not involved and does not know a switch happened.

The exception is a thread being preempted rather than yielding. There the
kernel has already saved the complete register state, vector registers
included, into a signal frame on that thread's own stack, and the switch out
happens from inside the handler. Resuming such a thread returns into the
handler, which returns, and `sigreturn` restores everything.

## 2. User-level threads versus kernel threads. Which is this?

A 1:1 runtime gives every thread of control its own kernel task - that is
pthreads on Linux, where `pthread_create` is `clone`. An N:1 runtime multiplexes
many threads of control onto one kernel task, which is this project. M:N puts N
threads of control across M kernel tasks and moves them between the tasks -
Go's runtime, Erlang's, Java's virtual threads.

The tradeoff is direct. 1:1 gets real parallelism and a scheduler that knows
about I/O and page faults, and pays a syscall for every switch and a stack
reservation for every thread. N:1 makes switching a handful of instructions and
threads nearly free, and gets one core and no protection at all against a
thread that genuinely blocks the kernel task. M:N gets both and is much harder,
because the runtime now has to schedule across real CPUs and cope with real
parallelism inside its own data structures.

## 3. Why is a green thread switch cheaper than a pthread switch?

Because it is not a syscall and does not involve the kernel scheduler. A pthread
handover over a condition variable costs a `futex` wait, a `futex` wake, a trip
through the kernel's scheduler to pick the other thread, and the privilege
transitions on both sides. A green handover is two `_longjmp`s and a run queue
pop.

Measured on a 12th Gen i5-1235U, median of 11 runs of 1,000,000 round trips:
**45.8 ns** per green handover against **2681.2 ns** for two pthreads over a
condition variable and **1381.7 ns** over a pipe - 58x and 30x. Thread creation:
**0.332 us** against **10.451 us**, 31x, because `go_spawn` takes a slot off a
free list while `pthread_create` asks the kernel for a stack and a task. Full
figures and machine description in `bench/results.md`.

## 4. How is preemption achieved without the kernel scheduler? Why sigaltstack?

`timer_create(CLOCK_MONOTONIC)` with a 10 ms interval delivers `SIGALRM`, and
the kernel interrupts whatever green thread is running to deliver it. That is
the whole mechanism: a signal is an upcall, and an upcall is all preemption
needs. `gth_tick_handler` in `src/preempt.c` counts the tick and switches to the
scheduler. `CLOCK_MONOTONIC` rather than `CLOCK_REALTIME` so that setting the
system clock cannot move a sleep deadline.

`sigaltstack` is in this project but not on the timer handler, and that is the
one place the implementation deviates from the plan it was built to. The signal
frame has to live somewhere, and if a thread is nearly out of stack, "somewhere"
is off the end of it - which is what an alternate stack is for. But the timer
handler does not return normally when it preempts: it switches away and its
frame must stay valid until that thread is scheduled again. An alternate stack
is one shared buffer, and the kernel places each new frame at the top of it
whenever the interrupted stack pointer is not already inside, so preempting a
second thread writes over the first thread's saved frame and resuming the first
thread restores the second thread's registers. So the timer handler runs on the
green thread's own stack, and the alternate stack carries the `SIGSEGV` handler
instead - the handler that reports stack overflow, and the one that genuinely
cannot run on the stack it is reporting about. Without `sigaltstack` there,
a stack overflow would fault, fault again trying to report it, and the process
would die with no explanation.

## 5. What is async-signal-safety, and what could not be called from the handler?

A signal handler runs on top of an arbitrary instruction of the interrupted
program, so anything that holds a lock or a static buffer may be halfway
through using it. Calling such a function from the handler either deadlocks
against a lock the interrupted code already holds, or corrupts state that was
mid-update. POSIX lists the functions that are safe; everything else is not.

In this codebase the ones that could not be called from `gth_tick_handler` are
`printf` and everything in stdio, which holds a lock and a buffer; `malloc`,
`free` and `realloc`, which hold the arena lock - `gth_netpoll_grow()` in
`netpoll.c` calls `realloc` and would deadlock if the handler ever allocated
while it was in progress; `perror` and `strerror`; and the runtime's own
`gth_wake` and `gth_rq_push`, which is why the handler refuses to switch while
a critical section is held. What the handler does call is `_setjmp`, `_longjmp`
and an integer increment. The `SIGSEGV` handler prints, and prints with `write`
and a hand-rolled hex formatter for the same reason.

## 6. Why is a critical section a sigprocmask here, and why the nesting counter?

It was a `sigprocmask` until M7 and now it is not, and both halves are worth
knowing. With one CPU and one runtime, the only thing that can interleave with
a run queue update is the timer handler, so masking `SIGALRM` around the update
is exactly an interrupt mask on a bare metal system - `sigprocmask` is the mask
register and `SIGALRM` is the interrupt. There is nothing to spin against
because there is no second CPU.

The counter is there because these sections nest: `gth_chan_send` takes one and
calls `gth_wake`, which takes another, and an inner section that unmasked on
exit would leave the outer one running with the timer live and the wait list
half unlinked. So the mask changes only on the 0 to 1 and 1 to 0 transitions,
never unconditionally. It lives in the TCB rather than in a global because a
thread may block while holding a section - every wait primitive does exactly
that - and the next thread to run must not inherit a depth it never entered.

M7 replaced the mask with deferred preemption: the counter is now an ordinary
userspace variable, the handler reads it and, if it is non-zero, records that a
preemption is owed and returns instead of switching, and `gth_exit_critical`
takes the deferred preemption when the last section is released. Nothing enters
the kernel. The cost is that preemption is delayed by the length of a critical
section rather than being impossible during it; every section here is a few
dozen instructions with no unbounded loop, so the delay is bounded and tiny
next to the 10 ms tick. It removed just over six `rt_sigprocmask` calls per
channel handover, measured with `strace -c`.

## 7. How does the scheduler pick the next thread in O(1)?

`gth_sched_pick()` in `src/sched.c`:

```c
p = 31 - __builtin_clz(grt.ready_bitmap);
```

One bit per priority level in a `uint32_t`, set when a level's queue becomes
non-empty and cleared when it empties. `__builtin_clz` counts leading zeros, so
`31 - clz` is the index of the most significant set bit, which is the highest
priority level that has anybody on it. On x86-64 it compiles to a single `bsr`
(or `lzcnt` with `-march=haswell` and up). It is O(1) in the number of threads
and in the number of levels: ten threads or ten thousand, it is one instruction.
`clz(0)` is undefined, so the empty case is tested first.

## 8. What is priority inversion, how does the mutex fix it, what does it cost?

Low holds a mutex. High blocks on it. Medium, which wants nothing from either,
is runnable and outranks Low, so the scheduler runs Medium - and the highest
priority thread in the system is now blocked behind the lowest for as long as
Medium feels like running. Mars Pathfinder shipped with this in 1997: a high
priority bus management task blocked on a mutex held by a low priority
meteorological task, with a medium priority communications task in between,
until the watchdog reset the spacecraft.

`gth_mutex_lock` in `src/sync.c` fixes it with priority inheritance: while High
is blocked on the mutex, Low runs at High's priority, so Medium cannot get in
front, and the boost is dropped on unlock. `test/t_sync.c` builds exactly this
arrangement and measures the wait - about 63 ms, which is Low's remaining work,
rather than Low's work plus Medium's.

The cost is real: lock and unlock now walk the waiter list to find the ceiling,
an owner's priority can change while it is queued which means moving it between
run queue levels, and the boost covers only this one mutex - a chain where the
owner is itself blocked on a second mutex is not propagated. That last one is a
genuine limitation of the implementation and not a subtlety of the technique.

## 9. Buffered versus unbuffered channels, mechanically. Where does the value live?

A buffered channel has a ring buffer. A send copies into it and returns; the
sender and the receiver need never be in the same place at the same time, and
the channel owns the value in between.

An unbuffered channel has no storage at all, so there is nowhere for the value
to live in between - which is why it is not a queue of size zero but a
synchronisation point that happens to carry a value. The sender parks itself on
the channel with a pointer to the value in **its own stack frame**, and the
first receiver to arrive copies straight out of that frame and unblocks the
sender. The value is only ever in the sender's frame and then the receiver's;
the channel holds a pointer, never the value. That is why the send does not
return until a receiver arrives - the return is the signal that the handoff
happened. `chan_try_send` and `chan_try_recv` in `src/chan.c`, and the waiter
node that carries the pointer is a stack local in `gth_chan_send`, valid for
exactly as long as the thread is parked, which is why the hot path allocates
nothing.

## 10. How does select avoid waking a thread twice?

`gth_select` registers one waiter node per case, on every channel at once, and
then blocks. The hazard is that two of those channels become ready before the
selecting thread gets the CPU back: the first sender hands over a value and
makes the thread runnable, and a second sender, running before the woken thread
does, finds the thread's other node still sitting on its channel and hands over
a second value that nothing will ever receive. A lost value in a CSP runtime is
the worst kind of bug because it looks like an application logic error.

The fix is a single `claimed` flag in the TCB, checked and set in
`chan_claim()`. The first claimer wins, every later one skips the node and drops
it, and the woken thread removes its remaining nodes from every channel before
its stack frame goes away. What makes the check-and-set indivisible is not an
atomic instruction - every caller holds a critical section, so the timer cannot
land between the read and the write. That is the whole mutual exclusion story
of this runtime: one CPU, one interrupt, mask the interrupt. Note what the race
is not: it is not two CPUs racing, it is two ordinary threads running at
different times against a thread that has been woken but has not run yet.

## 11. Why is a blocking syscall fatal to an N:1 runtime? How does Go solve it?

Because the kernel only sees one thread of execution. A green thread calling
`read()` on a socket with no data puts *the OS thread* to sleep, and the OS
thread is every green thread in the process: the scheduler stops, the timer
handler still fires but has nothing it can switch to, and ten thousand other
connections wait on one that has nothing to say.

`src/netpoll.c` fixes it by never letting the kernel block on our behalf. Every
fd is `O_NONBLOCK`, so a read with no data returns `EAGAIN`; `gth_read` then
registers the fd with `epoll` and parks the calling green thread, and the
scheduler unparks it when `epoll_wait` reports readiness. Inside the green
thread `gth_read` looked exactly like `read`; from outside, the process never
blocked.

Go does the same thing - a goroutine blocking on a socket is parked on a netpoll
descriptor and another goroutine runs on that OS thread - and then does the part
this runtime cannot: because it is M:N, a goroutine that blocks the OS thread
for real, in a genuine blocking syscall or a page fault, costs one of many
threads, and the runtime hands the remaining goroutines to another one.

## 12. Level-triggered versus edge-triggered epoll. Which and why?

Level-triggered, `src/netpoll.c`. Level triggering reports a state: if the fd
still has data at the next `epoll_wait`, it is reported again, so a reader that
consumes less than everything available simply gets told again. Edge triggering
reports a transition, which obliges every reader to drain the fd until `EAGAIN`
before going back to the poller; miss that once and the connection hangs with
data sitting in the socket buffer and no event ever coming to say so.

Edge triggering is faster when one thread multiplexes many fds, because ready
fds are not re-reported on every wait. That advantage is small here: interest is
deregistered the moment the parked thread is woken, so an fd is never in the
epoll set with nobody waiting on it, and there is nothing to re-report. Level
triggering costs almost nothing and removes a class of bug from every caller of
`gth_read`.

## 13. What happens on a stack overflow, and how was it made detectable?

Each stack in the pool has a `PROT_NONE` page immediately below it
(`src/stack.c`). Without it, running off the bottom of a 64 KiB stack writes
into the next thread's stack - silent corruption discovered minutes later
somewhere unrelated, which is the single worst failure mode a threading library
can have. With it, the first byte of overflow faults immediately at a known
address.

The `SIGSEGV` handler in `src/preempt.c` catches that fault on the alternate
signal stack, checks whether the faulting address falls in a guard page, and
reports "this is a green thread stack overflow, not a wild pointer" with the
address, using only `write` and `_exit`. It costs one page of address space per
thread and no resident memory at all. The main thread is the exception: it runs
on the process stack the kernel set up, which has its own guard region and grows
on demand.

## 14. What are the limits of this design, and what would M:N give you?

One core, and that is the big one. Every green thread shares one OS thread, so
the runtime cannot use a second CPU no matter how many are present. The echo
server benchmark had to pin the pthread server to one core to make the
comparison honest; unpinned, on twelve cores, the pthread server wins outright.

The rest: any genuinely blocking operation still stops everything - a page fault,
a `getaddrinfo`, an `open` on a slow filesystem, a `read` on a regular file,
none of which `epoll` covers. The thread pool is a fixed size chosen at
`gth_init`. There is no cancellation, so a thread parked on an fd cannot be
made to give up. glibc's locks know nothing about green threads, so two green
threads inside `malloc` at once would deadlock on the arena lock, and the
runtime avoids allocating anywhere a preempted thread can reach. The scheduler's
sleeper list is a linear walk, once per tick rather than per switch, which is
fine at ten thousand sleepers and would not be at a million.

M:N would fix the first and part of the second: several OS threads, green
threads migrating between them, so real parallelism and a thread that blocks the
kernel costing one worker rather than everything. The price is that every data
structure in this project would need real locking or lock-free algorithms,
because "one CPU, mask the interrupt" - the argument that makes the critical
sections, the claim flag in `select` and the whole scheduler correct - would
stop being true. That is a different and much larger project, and it is
deliberately not this one.
