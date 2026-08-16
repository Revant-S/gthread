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

    if (swapcontext(&self->ctx, &grt.sched_ctx) != 0) {
        perror("gthread: swapcontext");
        abort();
    }
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
    void *rv = t->entry(t->arg);

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

static void gth_sched_loop(void)
{
    for (;;) {
        struct gth *t;

        gth_wake_sleepers();
        t = gth_sched_pick();

        if (t == NULL) {
            /*
             * Nothing runnable right now. If someone is sleeping, the next
             * timer tick will change that, so wait for it - with sigsuspend
             * rather than a spin, because a spin here would burn a core to
             * discover the same thing 10 ms later, and would compete with the
             * thread it is waiting for on a real workload.
             *
             * sigsuspend is the right primitive and not just a convenient
             * one: the scheduler runs with SIGALRM blocked, so testing the
             * queue and then waiting are two steps that a tick could slip
             * between. sigsuspend unblocks and waits as one atomic operation,
             * which closes that window.
             */
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
            fputs("gthread: deadlock - no runnable threads and no timers "
                  "pending\n", stderr);
            pause();
            continue;
        }

        grt.cur  = t;
        t->state = GTH_RUNNING;
        if (swapcontext(&grt.sched_ctx, &t->ctx) != 0) {
            perror("gthread: swapcontext");
            abort();
        }
        grt.cur = NULL;

        /*
         * Back from the thread. What to do with it is entirely a function of
         * the state it set before switching, which is why no primitive needs
         * to touch the run queue on its way out.
         */
        switch (t->state) {
        case GTH_RUNNING:
            gth_rq_push(t); /* yielded, still runnable */
            break;
        case GTH_DONE:
            if (t->detached)
                gth_slot_release(t);
            break;
        default:
            break; /* BLOCKED or SLEEPING: parked on someone else's list */
        }
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
