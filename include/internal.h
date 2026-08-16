/*
 * internal.h - thread control block, run queue and scheduler internals.
 *
 * Not installed. Nothing outside src/ should include this; the public surface
 * is gthread.h and sync.h.
 */
#ifndef GTHREAD_INTERNAL_H
#define GTHREAD_INTERNAL_H

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
    ucontext_t ctx;        /* saved machine context, valid while not running */
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
    struct gth *threads;      /* the slot pool */
    size_t      max_threads;
    struct gth *free_slots;   /* free list, linked through gth.next */
    size_t      nlive;

    /* Run queue: one FIFO per priority level, plus a bitmap of non-empty ones. */
    struct gth *rq_head[GTH_PRIO_LEVELS];
    struct gth *rq_tail[GTH_PRIO_LEVELS];
    uint32_t    ready_bitmap;

    ucontext_t  sched_ctx;    /* the scheduler loop's own context */
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
};

extern struct gth_runtime grt;

/* stack.c */
int   gth_stack_pool_init(struct gth_stack_pool *p, size_t nslots, size_t usable);
void  gth_stack_pool_destroy(struct gth_stack_pool *p);
void *gth_stack_slot(const struct gth_stack_pool *p, size_t i);

/* preempt.c */
#define GTH_TICK_MS 10
int   gth_preempt_init(void);
void  gth_preempt_stop(void);
void  gth_enter_critical(void);
void  gth_exit_critical(void);

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
