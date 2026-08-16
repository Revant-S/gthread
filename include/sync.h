/*
 * sync.h - semaphores, mutexes, wait groups, channels and select.
 *
 * None of these are thread safe in the pthreads sense and they do not need to
 * be: there is one OS thread, so the only concurrency to defend against is
 * the scheduler taking the CPU away mid-update. Every one of them does that
 * by masking the timer rather than by spinning on an atomic, which is why
 * there is not a single lock-free algorithm in here.
 */
#ifndef GTHREAD_SYNC_H
#define GTHREAD_SYNC_H

#include <stddef.h>

struct gth; /* opaque: the thread control block, see internal.h */

/* ------------------------------------------------------------------ */
/* Counting semaphore                                                 */
/* ------------------------------------------------------------------ */

typedef struct gth_sem {
    int         count;
    struct gth *waiters;
} gth_sem_t;

void gth_sem_init(gth_sem_t *s, int count);
void gth_sem_wait(gth_sem_t *s);
int  gth_sem_trywait(gth_sem_t *s); /* 0 on success, -1/EAGAIN if it would block */
void gth_sem_post(gth_sem_t *s);

/* ------------------------------------------------------------------ */
/* Mutex with priority inheritance                                    */
/* ------------------------------------------------------------------ */

typedef struct gth_mutex {
    struct gth *owner;
    struct gth *waiters;
} gth_mutex_t;

void gth_mutex_init(gth_mutex_t *m);
/*
 * Returns 0, or -1 with errno set. EDEADLK means the caller already owns it:
 * this mutex is not recursive and says so instead of silently succeeding.
 */
int  gth_mutex_lock(gth_mutex_t *m);
int  gth_mutex_trylock(gth_mutex_t *m);
int  gth_mutex_unlock(gth_mutex_t *m); /* EPERM if the caller is not the owner */

/* ------------------------------------------------------------------ */
/* Wait group                                                         */
/* ------------------------------------------------------------------ */

typedef struct gth_waitgroup {
    int       count;
    int       nwaiters;
    gth_sem_t sem;
} gth_waitgroup_t;

void gth_wg_init(gth_waitgroup_t *wg);
void gth_wg_add(gth_waitgroup_t *wg, int delta);
void gth_wg_done(gth_waitgroup_t *wg);
void gth_wg_wait(gth_waitgroup_t *wg);

#endif /* GTHREAD_SYNC_H */
