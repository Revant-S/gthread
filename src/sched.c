/*
 * sched.c - run queue, the scheduler loop, and context switching.
 *
 * The switch path is deliberately thread -> scheduler -> thread, never
 * thread -> thread directly. A direct switch is one swapcontext fewer, but it
 * means the outgoing thread has to decide who runs next, which puts the
 * scheduling policy inside every blocking primitive. Going through a
 * scheduler context instead gives one place where the run queue is read, one
 * place where a dead thread's slot is reclaimed, and one place where the
 * runtime idles. The cost is a second switch per hop; the benefit is that
 * every primitive in sync.c and chan.c ends with the same two lines
 * (mark state, call gth_switch_to_sched) and none of them contains policy.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

struct gth_runtime grt;

/* ------------------------------------------------------------------ */
/* Run queue                                                          */
/* ------------------------------------------------------------------ */

void gth_rq_push(struct gth *t)
{
    int p = t->prio;

    t->next  = NULL;
    t->state = GTH_RUNNABLE;
    if (grt.rq_tail[p] != NULL)
        grt.rq_tail[p]->next = t;
    else
        grt.rq_head[p] = t;
    grt.rq_tail[p] = t;
    grt.ready_bitmap |= (uint32_t)1 << p;
}

/*
 * Highest priority first, FIFO within a level.
 *
 * __builtin_clz counts leading zeros, so 31 - clz(bitmap) is the index of the
 * most significant set bit: the highest priority level that has anybody on it.
 * On x86-64 this compiles to a single bsr (or lzcnt with -march=haswell and
 * up), which is the whole point - selection is O(1) in the number of threads,
 * not O(levels) and certainly not O(threads). clz(0) is undefined, hence the
 * explicit empty check first.
 */
static struct gth *gth_sched_pick(void)
{
    int p;
    struct gth *t;

    if (grt.ready_bitmap == 0)
        return NULL;

    p = 31 - __builtin_clz(grt.ready_bitmap);
    t = grt.rq_head[p];
    grt.rq_head[p] = t->next;
    if (grt.rq_head[p] == NULL) {
        grt.rq_tail[p] = NULL;
        grt.ready_bitmap &= ~((uint32_t)1 << p);
    }
    t->next = NULL;
    return t;
}

/*
 * Take a RUNNABLE thread back off the run queue. Only needed when a thread's
 * priority changes while it is queued (priority inheritance in sync.c), since
 * the level it is filed under is no longer the level it belongs at.
 *
 * The queue is a singly linked FIFO per level, so this is a walk of one
 * level's list. That is acceptable precisely because it is rare: it happens
 * on a priority boost, not on a switch. Making it O(1) would mean doubly
 * linking every thread and paying for the extra pointer update on the hot
 * path to speed up the cold one.
 */
static void gth_rq_remove(struct gth *t)
{
    int p = t->prio;
    struct gth **link = &grt.rq_head[p];
    struct gth  *prev = NULL;

    while (*link != NULL && *link != t) {
        prev = *link;
        link = &(*link)->next;
    }
    if (*link == NULL)
        return; /* not queued */

    *link = t->next;
    if (grt.rq_tail[p] == t)
        grt.rq_tail[p] = prev;
    if (grt.rq_head[p] == NULL) {
        grt.rq_tail[p] = NULL;
        grt.ready_bitmap &= ~((uint32_t)1 << p);
    }
    t->next = NULL;
}

/*
 * Change a thread's effective priority, moving it between run queue levels if
 * it is currently queued. Everything else - RUNNING, BLOCKED, SLEEPING - just
 * takes the new value, because those threads are filed by state rather than
 * by level and will be enqueued at the new priority when they next become
 * runnable.
 */
void gth_set_effective_prio(struct gth *t, int prio)
{
    if (t->prio == prio)
        return;
    if (t->state == GTH_RUNNABLE) {
        gth_rq_remove(t);
        t->prio = prio;
        gth_rq_push(t);
    } else {
        t->prio = prio;
    }
}

/* ------------------------------------------------------------------ */
/* Wait lists                                                         */
/* ------------------------------------------------------------------ */

void gth_waitq_push(struct gth **head, struct gth *t)
{
    t->next = NULL;
    while (*head != NULL)
        head = &(*head)->next;
    *head = t;
}

/*
 * Hand the resource to the most urgent waiter, not to the one that has been
 * waiting longest. Waking FIFO would mean a low priority thread can be
 * released ahead of a high priority one that is blocked on the same
 * semaphore, which is a priority inversion introduced by the runtime itself -
 * the exact thing the mutex in sync.c goes to some trouble to avoid.
 *
 * The scan is O(waiters). A per-priority bucket array in every semaphore
 * would make it O(1), at 32 pointers per semaphore; wait lists in this
 * runtime are short and semaphores are many, so the scan wins.
 */
struct gth *gth_waitq_pop_highest(struct gth **head)
{
    struct gth **best = NULL;
    struct gth **link = head;

    while (*link != NULL) {
        if (best == NULL || (*link)->prio > (*best)->prio)
            best = link;
        link = &(*link)->next;
    }
    if (best == NULL)
        return NULL;
    {
        struct gth *t = *best;
        *best = t->next;
        t->next = NULL;
        return t;
    }
}

void gth_waitq_remove(struct gth **head, struct gth *t)
{
    while (*head != NULL) {
        if (*head == t) {
            *head = t->next;
            t->next = NULL;
            return;
        }
        head = &(*head)->next;
    }
}

void gth_wake(struct gth *t)
{
    if (t->state == GTH_RUNNABLE || t->state == GTH_RUNNING)
        return;
    gth_rq_push(t);
}

/* ------------------------------------------------------------------ */
/* Slot pool                                                          */
/* ------------------------------------------------------------------ */

struct gth *gth_slot_acquire(void)
{
    struct gth *t = grt.free_slots;

    if (t == NULL) {
        errno = EAGAIN;
        return NULL;
    }
    grt.free_slots = t->next;
    t->next = NULL;
    grt.nlive++;
    return t;
}

void gth_slot_release(struct gth *t)
{
    int id = t->id;
    void *stack = t->stack;
    size_t stack_size = t->stack_size;

    /*
     * Wipe the block on release rather than on acquire so a use-after-exit
     * reference through a stale pointer sees GTH_FREE instead of plausible
     * looking leftovers.
     */
    memset(t, 0, sizeof(*t));
    t->id         = id;
    t->stack      = stack;
    t->stack_size = stack_size;
    t->state      = GTH_FREE;
    t->next       = grt.free_slots;
    grt.free_slots = t;
    grt.nlive--;
}

/* ------------------------------------------------------------------ */
/* Context switching                                                  */
/* ------------------------------------------------------------------ */

/*
 * Hand the CPU back to the scheduler. The caller has already set its own
 * state (RUNNING if it merely yielded, BLOCKED/SLEEPING/DONE otherwise) and
 * has already put itself on whatever list will wake it up again.
 *
 * swapcontext() saves the callee-saved registers, the stack pointer, the
 * program counter and the signal mask into the first argument, then installs
 * the second. Everything else - the caller-saved registers, the FPU state
 * that matters - is dead across a function call by the ABI, so there is
 * nothing else to save. The signal mask being part of the saved set is not a
 * detail: it is what lets M2 mask SIGALRM around a run queue update and have
 * the mask travel with the thread across a block, instead of leaking into
 * whoever runs next.
 */
void gth_switch_to_sched(void)
{
    struct gth *self = grt.cur;

    /*
     * errno has to be saved with the thread, and it is easy to miss.
     *
     * In glibc errno is thread local - one copy per OS thread - and this
     * runtime has exactly one OS thread, so all the green threads share it.
     * A thread that calls read(), gets -1, and is preempted before it looks
     * at errno will read whatever errno the threads that ran in between left
     * behind. That is a bug that shows up as a socket error with an
     * impossible cause, once every few hours, under load.
     *
     * Every switch in the runtime funnels through this function, including
     * the involuntary one from the timer handler, so saving it here covers
     * every path. It is the same argument as the one for saving registers:
     * errno is part of the thread's state, it just happens to live in a
     * global instead of a register.
     */
    self->saved_errno = errno;

    /*
     * Announce that a switch is in flight before touching swapcontext, and
     * not after. swapcontext saves this thread's registers into self->ctx and
     * only then installs the scheduler's signal mask - the two are separate
     * steps and the timer is still live between them. A tick landing there
     * would find a perfectly ordinary running thread, call this same function
     * from the handler, and overwrite the context that was already saved a
     * few instructions ago. The thread would then have two saved contexts
     * describing two different points in its own switch, and whichever one
     * ran second would corrupt the other.
     *
     * The flag is cleared on the far side, when this thread has really been
     * resumed. Between the two the handler declines to preempt, which costs
     * at most one 10 ms slice and is the only thing that makes a context
     * switch atomic with respect to the timer.
     */
    grt.switching = 1;

    /*
     * Hand the scheduler the outgoing thread explicitly rather than letting
     * it work out for itself who just left.
     *
     * The obvious implementation is for the scheduler to keep the thread it
     * resumed in a local variable and deal with it on the far side of its own
     * swapcontext. That works for every switch except the first one: the
     * first entry into the scheduler does not return from a swapcontext at
     * all, it starts gth_sched_loop() from the top, so there is no local
     * variable and no far side. The thread that made that first switch is
     * simply forgotten, and if it happened to be still runnable - preempted
     * rather than blocked - it is never queued again and never runs again.
     *
     * It took a while to see because it needs a thread to survive a full
     * 10 ms tick before its first blocking call. main() spawning five
     * thousand threads does exactly that; main() spawning three does not.
     */
    grt.prev = self;

    if (swapcontext(&self->ctx, &grt.sched_ctx) != 0) {
        perror("gthread: swapcontext");
        abort();
    }

    /* Running again, and this time really on this thread's own stack. */
    grt.switching = 0;
    errno = self->saved_errno;
}

/*
 * Park the caller. The caller must already be on the wait list that will wake
 * it, and must be holding a critical section, or a wakeup delivered between
 * joining the list and switching would be lost.
 */
void gth_block(void)
{
    grt.cur->state = GTH_BLOCKED;
    gth_switch_to_sched();
}

/*
 * Every green thread starts here.
 *
 * makecontext() passes its arguments as int, which was fine in 1995 and is a
 * portability wart now: a struct gth * is 64 bits and an int argument is 32,
 * so the pointer cannot go through in one piece. Splitting it into two ints
 * and reassembling here is the standard workaround and is what glibc's own
 * documentation suggests. The casts go through uint32_t on the way back so
 * that a pointer with the top bit of either half set is not sign extended
 * into garbage.
 *
 * The other job of a trampoline is that a green thread must never return off
 * the end of its own stack. Below entry() there is no caller: the stack was
 * built by makecontext and returning past it lands on whatever bytes are
 * there. Calling gth_exit() here means the thread's last act is always a
 * clean switch back to the scheduler.
 */
static void gth_trampoline(int lo, int hi)
{
    struct gth *t = (struct gth *)(((uintptr_t)(uint32_t)hi << 32) |
                                   (uintptr_t)(uint32_t)lo);
    void *rv;

    grt.switching = 0; /* first entry into this thread, see gth_sched_loop() */
    rv = t->entry(t->arg);

    gth_exit(rv);
    abort(); /* gth_exit does not return */
}

void gth_thread_setup_context(struct gth *t)
{
    uintptr_t p = (uintptr_t)t;

    if (getcontext(&t->ctx) != 0) {
        perror("gthread: getcontext");
        abort();
    }
    t->ctx.uc_stack.ss_sp   = t->stack;
    t->ctx.uc_stack.ss_size = t->stack_size;

    /*
     * getcontext() snapshots the caller's signal mask, and the caller is
     * go_spawn() running inside a critical section with SIGALRM blocked. Left
     * alone, the new thread would start life with the timer masked and a
     * critical depth of zero - unpreemptable forever, and a tight loop in it
     * would hang the process. The mask has to be built to match the state the
     * thread actually starts in, not the state of whoever created it.
     */
    sigdelset(&t->ctx.uc_sigmask, SIGALRM);
    /*
     * uc_link is NULL on purpose. It is the context to resume when the entry
     * function returns, and a thread that reaches the end of gth_trampoline
     * has already switched away from inside gth_exit(). If control ever did
     * fall through to a NULL uc_link the process would exit, which is a loud
     * failure rather than a quiet one.
     */
    t->ctx.uc_link = NULL;
    makecontext(&t->ctx, (void (*)(void))gth_trampoline, 2,
                (int)(uint32_t)p, (int)(uint32_t)(p >> 32));
    t->started = 1;
}

/* ------------------------------------------------------------------ */
/* The scheduler                                                      */
/* ------------------------------------------------------------------ */

/*
 * Move every sleeper whose deadline has passed back onto the run queue.
 *
 * The comparison is written as a subtraction cast to signed, not as
 * now >= deadline, because the tick counter wraps. At 100 Hz a uint32_t wraps
 * after roughly 497 days, which is exactly the kind of thing that is never
 * tested and eventually happens; after the wrap a plain >= would consider
 * every future deadline already expired and every sleep would return
 * immediately. The subtraction is done in unsigned arithmetic, where wrapping
 * is defined, and the result reinterpreted as signed: it stays correct as
 * long as no deadline is more than 2^31 ticks away, which is 248 days.
 *
 * The walk is O(number of sleepers), so it is deliberately done at most once
 * per tick rather than once per switch. With 10k sleeping threads that is a
 * 10k element walk every 10 ms, not on every context switch. A timer wheel or
 * a heap would make it O(log n), and if the sleeper list ever became the
 * bottleneck that is the fix; measured against the switch path it is not
 * close.
 */
static void gth_wake_sleepers(void)
{
    uint32_t now = (uint32_t)grt.ticks;
    struct gth **link;

    if (now == grt.last_wake_scan)
        return;
    grt.last_wake_scan = now;

    link = &grt.sleepers;
    while (*link != NULL) {
        struct gth *t = *link;

        if ((int32_t)(now - t->wake_tick) >= 0) {
            *link = t->next;
            t->next = NULL;
            gth_rq_push(t);
        } else {
            link = &t->next;
        }
    }
}

/*
 * How long the runtime may wait before it has to look at the clock again:
 * until the earliest sleep deadline, or forever if nobody is sleeping.
 */
static int gth_idle_timeout_ms(void)
{
    uint32_t now = (uint32_t)grt.ticks;
    const struct gth *t;
    int32_t soonest = 0;
    int have = 0;

    for (t = grt.sleepers; t != NULL; t = t->next) {
        int32_t delta = (int32_t)(t->wake_tick - now);

        if (delta < 0)
            delta = 0;
        if (!have || delta < soonest) {
            soonest = delta;
            have    = 1;
        }
    }
    if (!have)
        return -1;
    return (int)soonest * GTH_TICK_MS;
}

static void gth_sched_loop(void)
{
    for (;;) {
        struct gth *t;

        /*
         * Deal with whoever just gave up the CPU. What happens to it is
         * entirely a function of the state it set before switching, which is
         * why no blocking primitive has to touch the run queue on its way
         * out: it marks itself and leaves.
         */
        if (grt.prev != NULL) {
            struct gth *prev = grt.prev;

            grt.prev = NULL;
            switch (prev->state) {
            case GTH_RUNNING:
                gth_rq_push(prev); /* yielded or was preempted, still runnable */
                break;
            case GTH_DONE:
                if (prev->detached)
                    gth_slot_release(prev);
                break;
            default:
                break; /* BLOCKED or SLEEPING: parked on someone else's list */
            }
        }

        gth_wake_sleepers();

        /*
         * Poll the netpoller before picking. With runnable threads the wait
         * is zero - I/O readiness is checked, nobody is kept waiting for it.
         * With nothing runnable, this call is where the process actually
         * sleeps, bounded by the next timer deadline so a sleeping thread is
         * not held up by a quiet socket.
         *
         * The guard matters: a program that never touches an fd must not pay
         * a syscall per scheduling pass for a poller it is not using, and the
         * context switch benchmark is exactly such a program.
         */
        if (grt.netpoll_parked > 0)
            gth_netpoll(grt.ready_bitmap != 0 ? 0 : gth_idle_timeout_ms());

        t = gth_sched_pick();

        if (t == NULL) {
            /*
             * Nothing runnable right now. If threads are parked on I/O, the
             * epoll_pwait above has already done the waiting, so go round
             * again. Otherwise, if someone is sleeping, wait for the tick -
             * with sigsuspend rather than a spin, because a spin here would
             * burn a core to discover the same thing 10 ms later, and would
             * compete with the thread it is waiting for on a real workload.
             *
             * sigsuspend is the right primitive and not just a convenient
             * one: the scheduler runs with SIGALRM blocked, so testing the
             * queue and then waiting are two steps that a tick could slip
             * between. sigsuspend unblocks and waits as one atomic operation,
             * which closes that window.
             */
            if (grt.netpoll_parked > 0)
                continue;
            if (grt.sleepers != NULL) {
                sigset_t idle;
                sigprocmask(SIG_SETMASK, NULL, &idle);
                sigdelset(&idle, SIGALRM);
                sigsuspend(&idle); /* always returns -1/EINTR */
                continue;
            }
            /*
             * Nothing runnable, nothing sleeping, nothing that could ever
             * make a thread runnable again. Every remaining thread is waiting
             * for another green thread that will never run. Say so and stop:
             * a deadlock that hangs quietly is a bug report nobody can act
             * on, and a deadlock that spins looks like a performance problem.
             */
            fputs("gthread: deadlock - no runnable threads, no timers "
                  "pending and no I/O outstanding\n", stderr);
            pause();
            continue;
        }

        /*
         * The scheduler runs with SIGALRM blocked, which is supposed to make
         * this whole loop safe from the timer. It is not quite enough, and
         * the gap is worth understanding because it took a 5,000 connection
         * test to find it.
         *
         * swapcontext() does not install the target's registers and the
         * target's signal mask at the same instant. glibc sets the mask first
         * - one rt_sigprocmask carrying the incoming context's uc_sigmask -
         * and only then loads the registers and jumps. For a handful of
         * instructions the CPU is therefore still running the scheduler, on
         * the scheduler's stack, but with the *incoming thread's* mask, in
         * which SIGALRM is unblocked. grt.cur already names the incoming
         * thread, because that store has to happen before the switch.
         *
         * A tick delivered in that window lands on the scheduler's stack, and
         * the handler, seeing a non-NULL grt.cur, switches out: it writes the
         * scheduler's own registers into t->ctx, on top of the context it was
         * halfway through resuming, and jumps into a scheduler context that is
         * being overwritten. What comes out the far side is a thread whose
         * saved context is a partly written signal frame, which resumes into
         * an endless sigreturn loop or a jump to address zero.
         *
         * It is a window of maybe ten instructions against a 10 ms timer, so
         * it happens once in a few million switches - which is to say never
         * during development and constantly under load.
         *
         * The flag closes it. The handler treats "a switch is in flight" the
         * same way it treats "the scheduler is running": there is nothing
         * safe to preempt, so it returns and the tick is simply lost. It is
         * cleared by the thread once it is genuinely running, at the two
         * places a thread can come back to life - after the swapcontext in
         * gth_switch_to_sched(), and at the top of gth_trampoline() for a
         * thread starting for the first time.
         */
        grt.switching = 1;
        grt.cur  = t;
        t->state = GTH_RUNNING;
        if (swapcontext(&grt.sched_ctx, &t->ctx) != 0) {
            perror("gthread: swapcontext");
            abort();
        }
        grt.cur = NULL;
        /* The thread that just left is in grt.prev; the top of the loop has
         * it. Nothing is done with t here, because control does not
         * necessarily come back through this point - the first entry into
         * this loop arrives at the top, not here. */
    }
}

static void gth_sched_entry(void)
{
    gth_sched_loop();
    abort(); /* the scheduler loop never returns */
}

/*
 * Build the scheduler's own context. It gets its own stack for the same
 * reason the kernel's scheduler does not run on a user stack: it must be able
 * to run when the thread that just gave up the CPU has none left to spare,
 * and its frames must not be part of any thread's saved state.
 */
void gth_sched_bootstrap(void)
{
    if (getcontext(&grt.sched_ctx) != 0) {
        perror("gthread: getcontext");
        abort();
    }
    grt.sched_ctx.uc_stack.ss_sp   = gth_stack_slot(&grt.sched_pool, 0);
    grt.sched_ctx.uc_stack.ss_size = grt.sched_pool.usable;
    grt.sched_ctx.uc_link          = NULL;

    /*
     * The scheduler always runs with SIGALRM blocked, and the mask is set
     * here, once, because swapcontext restores it on every entry into the
     * scheduler for free.
     *
     * This is not tidiness, it is a correctness requirement. Between
     * "grt.cur = t" and the swapcontext that actually installs t's registers
     * there are a few instructions during which grt.cur names a thread that
     * is not running. A tick delivered in that window would find cur non-NULL
     * and switch out - saving the scheduler's own registers into t->ctx, on
     * top of the context it was about to resume. Blocking the timer for the
     * whole scheduler removes the window instead of trying to make it small.
     */
    sigaddset(&grt.sched_ctx.uc_sigmask, SIGALRM);

    makecontext(&grt.sched_ctx, gth_sched_entry, 0);
}
