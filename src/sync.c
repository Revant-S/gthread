/*
 * sync.c - semaphore, mutex with priority inheritance, wait group.
 */
#include <errno.h>
#include <stddef.h>

#include "internal.h"
#include "sync.h"

/*
 * If the thread we just made runnable outranks us, give it the CPU now rather
 * than at the next tick. Without this, a fixed priority scheduler is only
 * fixed priority on a 10 ms grid: post to a high priority waiter and then
 * carry on computing for another 9 ms and the priority meant nothing.
 *
 * This is called after the critical section is released, so the switch
 * happens with the timer live.
 */
static void gth_yield_to(struct gth *t)
{
    if (t != NULL && t->prio > grt.cur->prio)
        gth_yield();
}

/* ------------------------------------------------------------------ */
/* Semaphore                                                          */
/* ------------------------------------------------------------------ */

void gth_sem_init(gth_sem_t *s, int count)
{
    s->count   = count;
    s->waiters = NULL;
}

void gth_sem_wait(gth_sem_t *s)
{
    struct gth *self = grt.cur;

    gth_enter_critical();
    /*
     * while, not if. The count can be taken again between the post that woke
     * this thread and this thread actually running - gth_sem_post() hands out
     * a token, not the token - so the condition has to be rechecked. This is
     * the same reason a condition variable wait is always in a loop.
     */
    while (s->count == 0) {
        gth_waitq_push(&s->waiters, self);
        gth_block();
    }
    s->count--;
    gth_exit_critical();
}

int gth_sem_trywait(gth_sem_t *s)
{
    int ok;

    gth_enter_critical();
    ok = s->count > 0;
    if (ok)
        s->count--;
    gth_exit_critical();
    if (!ok) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

void gth_sem_post(gth_sem_t *s)
{
    struct gth *woken;

    gth_enter_critical();
    s->count++;
    woken = gth_waitq_pop_highest(&s->waiters);
    if (woken != NULL)
        gth_wake(woken);
    gth_exit_critical();

    gth_yield_to(woken);
}

/* ------------------------------------------------------------------ */
/* Mutex                                                              */
/* ------------------------------------------------------------------ */

void gth_mutex_init(gth_mutex_t *m)
{
    m->owner   = NULL;
    m->waiters = NULL;
}

/*
 * The priority the owner has to run at: its own, or that of the most urgent
 * thread queued behind it, whichever is higher.
 */
static int gth_mutex_ceiling(const gth_mutex_t *m, const struct gth *owner)
{
    int best = owner->base_prio;
    const struct gth *w;

    for (w = m->waiters; w != NULL; w = w->next) {
        if (w->prio > best)
            best = w->prio;
    }
    return best;
}

/*
 * Priority inversion, and the fix.
 *
 * Low holds the mutex. High blocks on it. Medium, which wants nothing from
 * either of them, is runnable and outranks Low, so the scheduler runs Medium.
 * High is now waiting on Low, and Low is waiting for a CPU that Medium will
 * not give up: the highest priority thread in the system is blocked behind
 * the lowest, for as long as Medium feels like running. This is what put
 * Mars Pathfinder into a reset loop in 1997 - a high priority bus management
 * task blocked on a mutex held by a low priority meteorological task, with a
 * medium priority comms task in between, until the watchdog fired.
 *
 * The fix is priority inheritance: while High is blocked on this mutex, Low
 * runs at High's priority, so Medium cannot get in front of it. Low finishes
 * its critical section quickly, unlocks, drops back to its own priority, and
 * High proceeds. The cost is real and worth stating: every lock and unlock
 * now walks the waiter list, an owner's priority can change while it is on
 * the run queue (which means moving it between queue levels), and the boost
 * only covers this one mutex - a chain where the owner is itself blocked on
 * a second mutex is not propagated here.
 */
int gth_mutex_lock(gth_mutex_t *m)
{
    struct gth *self = grt.cur;

    gth_enter_critical();

    if (m->owner == NULL) {
        m->owner = self;
        gth_exit_critical();
        return 0;
    }
    if (m->owner == self) {
        /*
         * Recursive locking is a bug, not a feature. A recursive mutex makes
         * "I hold this lock" stop meaning "the invariant is restored", and
         * the second acquisition is almost always a function that did not
         * expect to be called with the lock held.
         */
        gth_exit_critical();
        errno = EDEADLK;
        return -1;
    }

    gth_waitq_push(&m->waiters, self);
    if (m->owner->prio < self->prio)
        gth_set_effective_prio(m->owner, self->prio);
    gth_block();

    /*
     * Woken means the lock was handed over directly by gth_mutex_unlock():
     * this thread is already the owner. Waking a waiter and having it race
     * for the lock against threads that never queued would let a newcomer
     * steal it, which is both unfair and unbounded.
     */
    gth_exit_critical();
    return 0;
}

int gth_mutex_trylock(gth_mutex_t *m)
{
    int ok;

    gth_enter_critical();
    ok = (m->owner == NULL);
    if (ok)
        m->owner = grt.cur;
    gth_exit_critical();
    if (!ok) {
        errno = EBUSY;
        return -1;
    }
    return 0;
}

int gth_mutex_unlock(gth_mutex_t *m)
{
    struct gth *self = grt.cur;
    struct gth *next;

    gth_enter_critical();
    if (m->owner != self) {
        gth_exit_critical();
        errno = EPERM;
        return -1;
    }

    /* Drop any boost this thread was given while it held the lock. */
    gth_set_effective_prio(self, self->base_prio);

    next = gth_waitq_pop_highest(&m->waiters);
    m->owner = next;
    if (next != NULL) {
        /*
         * The new owner inherits from whoever is still queued behind it, for
         * the same reason the old owner did.
         */
        gth_set_effective_prio(next, gth_mutex_ceiling(m, next));
        gth_wake(next);
    }
    gth_exit_critical();

    gth_yield_to(next);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wait group                                                         */
/* ------------------------------------------------------------------ */

void gth_wg_init(gth_waitgroup_t *wg)
{
    wg->count    = 0;
    wg->nwaiters = 0;
    gth_sem_init(&wg->sem, 0);
}

void gth_wg_add(gth_waitgroup_t *wg, int delta)
{
    gth_enter_critical();
    wg->count += delta;
    gth_exit_critical();
}

void gth_wg_done(gth_waitgroup_t *wg)
{
    int release = 0;

    gth_enter_critical();
    if (--wg->count <= 0) {
        release = wg->nwaiters;
        wg->nwaiters = 0;
    }
    gth_exit_critical();

    /*
     * The posts happen outside the critical section because gth_sem_post()
     * can switch, and switching with the group's own fields half updated is
     * exactly what the section is there to prevent.
     */
    while (release-- > 0)
        gth_sem_post(&wg->sem);
}

void gth_wg_wait(gth_waitgroup_t *wg)
{
    int must_wait;

    gth_enter_critical();
    must_wait = wg->count > 0;
    if (must_wait)
        wg->nwaiters++;
    gth_exit_critical();

    if (must_wait)
        gth_sem_wait(&wg->sem);
}
