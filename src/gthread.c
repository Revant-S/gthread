/*
 * gthread.c - the public API: init, spawn, yield, join, exit.
 *
 * Everything here is a thin layer over sched.c. The one design decision worth
 * defending is in gth_init(): the process's initial thread is turned into
 * green thread 0 rather than being kept outside the runtime. It costs one
 * slot and it means main() is an ordinary green thread that can spawn, block
 * and be preempted like any other, instead of being a special case that every
 * primitive has to check for.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#define GTH_DEFAULT_MAX_THREADS 1024
#define GTH_DEFAULT_STACK_SIZE  (64 * 1024)
#define GTH_SCHED_STACK_SIZE    (64 * 1024)

int gth_init_config(const gth_config_t *cfg)
{
    size_t nthreads = (cfg != NULL && cfg->max_threads != 0)
                          ? cfg->max_threads : GTH_DEFAULT_MAX_THREADS;
    size_t stack    = (cfg != NULL && cfg->stack_size != 0)
                          ? cfg->stack_size : GTH_DEFAULT_STACK_SIZE;
    struct gth *main_thread;
    size_t i;

    if (grt.initialised) {
        errno = EBUSY;
        return -1;
    }
    memset(&grt, 0, sizeof(grt));

    grt.threads = calloc(nthreads, sizeof(*grt.threads));
    if (grt.threads == NULL)
        return -1;
    grt.max_threads = nthreads;

    if (gth_stack_pool_init(&grt.pool, nthreads, stack) != 0) {
        free(grt.threads);
        grt.threads = NULL;
        return -1;
    }
    if (gth_stack_pool_init(&grt.sched_pool, 1, GTH_SCHED_STACK_SIZE) != 0) {
        gth_stack_pool_destroy(&grt.pool);
        free(grt.threads);
        grt.threads = NULL;
        return -1;
    }

    /*
     * Build the free list back to front so that slots are handed out in
     * ascending id order. Nothing depends on it, but reading a trace where
     * ids climb is easier than reading one where they count down.
     */
    for (i = nthreads; i-- > 0;) {
        struct gth *t = &grt.threads[i];
        t->id         = (int)i;
        t->state      = GTH_FREE;
        t->stack      = gth_stack_slot(&grt.pool, i);
        t->stack_size = grt.pool.usable;
        t->next       = grt.free_slots;
        grt.free_slots = t;
    }

    /*
     * Slot 0 becomes the caller. It has no stack of its own: it runs on the
     * process stack the kernel set up, which is why stack/stack_size are
     * cleared here. That stack has a kernel-managed guard region and grows on
     * demand, so it is the one green thread that cannot overflow into a
     * neighbour.
     */
    main_thread = gth_slot_acquire();
    main_thread->stack      = NULL;
    main_thread->stack_size = 0;
    main_thread->state      = GTH_RUNNING;
    main_thread->prio       = GTH_PRIO_DEFAULT;
    main_thread->base_prio  = GTH_PRIO_DEFAULT;
    main_thread->started    = 1;
    grt.cur = main_thread;

    gth_sched_bootstrap();
    grt.initialised = 1;
    return 0;
}

int gth_init(void)
{
    return gth_init_config(NULL);
}

void gth_shutdown(void)
{
    if (!grt.initialised)
        return;
    gth_stack_pool_destroy(&grt.pool);
    gth_stack_pool_destroy(&grt.sched_pool);
    free(grt.threads);
    memset(&grt, 0, sizeof(grt));
}

int go_spawn_ex(void *(*fn)(void *), void *arg, int prio, int detached)
{
    struct gth *t;

    if (fn == NULL || prio < GTH_PRIO_MIN || prio > GTH_PRIO_MAX) {
        errno = EINVAL;
        return -1;
    }

    t = gth_slot_acquire();
    if (t == NULL)
        return -1; /* pool exhausted, errno is EAGAIN */

    t->entry     = fn;
    t->arg       = arg;
    t->prio      = prio;
    t->base_prio = prio;
    t->detached  = detached;
    t->joiner    = NULL;
    t->retval    = NULL;
    gth_thread_setup_context(t);
    gth_rq_push(t);
    return t->id;
}

int go_spawn(void *(*fn)(void *), void *arg)
{
    return go_spawn_ex(fn, arg, GTH_PRIO_DEFAULT, 0);
}

int go_spawn_detached(void *(*fn)(void *), void *arg)
{
    return go_spawn_ex(fn, arg, GTH_PRIO_DEFAULT, 1);
}

void gth_yield(void)
{
    /*
     * State stays RUNNING: that is the signal to the scheduler that this
     * thread is still runnable and belongs back on the queue.
     */
    gth_switch_to_sched();
}

void gth_exit(void *retval)
{
    struct gth *self = grt.cur;

    self->retval = retval;
    self->state  = GTH_DONE;
    if (self->joiner != NULL) {
        gth_wake(self->joiner);
        self->joiner = NULL;
    }
    gth_switch_to_sched();
    abort(); /* the scheduler never resumes a DONE thread */
}

int gth_join(int tid, void **retval)
{
    struct gth *t;
    struct gth *self = grt.cur;

    if (tid < 0 || (size_t)tid >= grt.max_threads) {
        errno = ESRCH;
        return -1;
    }
    t = &grt.threads[tid];
    if (t->state == GTH_FREE || t == self || t->detached) {
        errno = EINVAL;
        return -1;
    }
    if (t->joiner != NULL) {
        errno = EINVAL; /* one joiner per thread, like pthreads */
        return -1;
    }

    if (t->state != GTH_DONE) {
        t->joiner   = self;
        self->state = GTH_BLOCKED;
        gth_switch_to_sched();
    }

    /*
     * The joined thread stayed as a zombie slot precisely so that its retval
     * would still be here. Reclaiming it is the joiner's job.
     */
    if (retval != NULL)
        *retval = t->retval;
    gth_slot_release(t);
    return 0;
}

int gth_self(void)
{
    return grt.cur->id;
}

int gth_getprio(void)
{
    return grt.cur->base_prio;
}

int gth_setprio(int prio)
{
    if (prio < GTH_PRIO_MIN || prio > GTH_PRIO_MAX) {
        errno = EINVAL;
        return -1;
    }
    grt.cur->base_prio = prio;
    grt.cur->prio      = prio;
    return 0;
}
