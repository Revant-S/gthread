/*
 * internal.h - thread control block, run queue and scheduler internals.
 *
 * Not installed. Nothing outside src/ should include this; the public surface
 * is gthread.h and sync.h.
 */
#ifndef GTHREAD_INTERNAL_H
#define GTHREAD_INTERNAL_H

#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

#include "gthread.h"

enum gth_state {
    GTH_FREE = 0,  /* slot is on the free list */
    GTH_RUNNABLE,  /* on the run queue, waiting for the CPU */
    GTH_RUNNING,   /* currently executing, exactly one thread is in this state */
    GTH_BLOCKED,   /* on some wait list: join, semaphore, channel, netpoll */
    GTH_SLEEPING,  /* on the sleeper list with a deadline */
    GTH_DONE       /* returned, retval valid, slot alive until joined */
};

/*
 * The thread control block. One per slot in the fixed pool.
 *
 * next is intrusive and single: a thread is on exactly one list at a time
 * (run queue, sleeper list, or one wait list), so a single link is enough.
 * gth_select() is the exception and it does not use this field - see chan.c.
 */
struct gth {
    /*
     * Two ways to enter this thread, for two different situations.
     *
     * jb is the steady state one: _setjmp/_longjmp, no syscall, used for every
     * switch after the first. ctx is used exactly once per thread, to build a
     * stack that has never been run on - there is no way to manufacture a
     * jmp_buf pointing at a fresh stack in portable C, and makecontext is the
     * POSIX tool for precisely that job. See gth_sched_loop().
     */
    jmp_buf    jb;
    ucontext_t ctx;        /* first entry only, see above */
    void      *stack;      /* lowest usable byte of this slot's stack */
    size_t     stack_size;
    int        id;
    enum gth_state state;
    int        prio;       /* effective priority, may be boosted by a mutex */
    int        base_prio;  /* what prio returns to when the boost is dropped */
    uint32_t   wake_tick;  /* deadline for GTH_SLEEPING, in scheduler ticks */
    struct gth *next;      /* intrusive link, see above */
    struct gth *joiner;    /* thread parked in gth_join() on this one */
    void      *retval;
    void    *(*entry)(void *);
    void      *arg;
    int        detached;
    int        started;    /* has the context been entered at least once */
    int        saved_errno; /* errno is a per-OS-thread global, see sched.c */
    int        crit;       /* critical section nesting depth, see preempt.c */
    /*
     * Set by whichever channel operation completes this thread's wait, so
     * that a thread registered on several channels by gth_select() is woken
     * by exactly one of them. Cleared by the thread just before it blocks.
     * See chan_claim() in chan.c.
     */
    int        claimed;
};

/*
 * One mmap'd region carved into nslots stacks, each preceded by a PROT_NONE
 * guard page. See stack.c.
 */
struct gth_stack_pool {
    void  *base;
    size_t nslots;
    size_t usable; /* writable bytes per stack */
    size_t guard;  /* PROT_NONE bytes below each stack, one page */
    size_t span;   /* guard + usable, the stride between slots */
};

struct gth_runtime {
    int         initialised;
    struct gth *cur;          /* running thread, NULL while in the scheduler */
    struct gth *prev;         /* thread that just gave up the CPU, unprocessed */
    struct gth *threads;      /* the slot pool */
    size_t      max_threads;
    struct gth *free_slots;   /* free list, linked through gth.next */
    size_t      nlive;

    /* Run queue: one FIFO per priority level, plus a bitmap of non-empty ones. */
    struct gth *rq_head[GTH_PRIO_LEVELS];
    struct gth *rq_tail[GTH_PRIO_LEVELS];
    uint32_t    ready_bitmap;

    jmp_buf     sched_jb;     /* the scheduler's steady state entry point */
    int         sched_started; /* sched_jb is only valid once the loop has run */
    ucontext_t  sched_ctx;    /* used once, to start the scheduler loop */
    struct gth_stack_pool pool;       /* green thread stacks */
    struct gth_stack_pool sched_pool; /* one slot: the scheduler's own stack */

    /*
     * Written by the SIGALRM handler, read everywhere. sig_atomic_t is the
     * only integer type the C standard promises can be accessed atomically
     * with respect to a signal handler; volatile stops the compiler from
     * caching it across the loop in gth_sleep().
     */
    volatile sig_atomic_t ticks;
    uint32_t    last_wake_scan; /* tick at which the sleeper list was last walked */
    struct gth *sleepers;       /* unsorted list of GTH_SLEEPING threads */

    /*
     * Set while the scheduler is handing the CPU to a thread but has not
     * arrived there yet. See the long comment in gth_sched_loop().
     */
    volatile sig_atomic_t switching;

    /*
     * Deferred preemption. The handler sets pending when it cannot switch
     * because a critical section is held; gth_exit_critical() takes the
     * preemption when the last one is released. alarm_blocked records that a
     * switch left a signal handler without returning from it, so the
     * scheduler knows it has to unblock SIGALRM by hand.
     */
    volatile sig_atomic_t preempt_pending;
    volatile sig_atomic_t alarm_blocked;

    int         epfd;           /* epoll instance, -1 when netpoll is unused */
    size_t      netpoll_parked; /* threads waiting on fd readiness */
};

extern struct gth_runtime grt;

/* stack.c */
int   gth_stack_pool_init(struct gth_stack_pool *p, size_t nslots, size_t usable);
void  gth_stack_pool_destroy(struct gth_stack_pool *p);
void *gth_stack_slot(const struct gth_stack_pool *p, size_t i);

/* netpoll.c */
int   gth_netpoll_init(void);
void  gth_netpoll_shutdown(void);
void  gth_netpoll(int timeout_ms);

/* preempt.c */
#define GTH_TICK_MS 10
int   gth_preempt_init(void);
void  gth_preempt_stop(void);
void  gth_enter_critical(void);
void  gth_exit_critical(void);
void  gth_unblock_alarm(void);

/* sched.c */
void  gth_rq_push(struct gth *t);
void  gth_wake(struct gth *t);
void  gth_set_effective_prio(struct gth *t, int prio);
void  gth_waitq_push(struct gth **head, struct gth *t);
struct gth *gth_waitq_pop_highest(struct gth **head);
void  gth_waitq_remove(struct gth **head, struct gth *t);
void  gth_block(void);
void  gth_switch_to_sched(void);
void  gth_sched_bootstrap(void);
void  gth_thread_setup_context(struct gth *t);
void  gth_slot_release(struct gth *t);
struct gth *gth_slot_acquire(void);

#endif /* GTHREAD_INTERNAL_H */
