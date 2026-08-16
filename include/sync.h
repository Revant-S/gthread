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

/* ------------------------------------------------------------------ */
/* Channels                                                           */
/* ------------------------------------------------------------------ */

typedef struct gth_chan gth_chan_t;

/*
 * capacity 0 gives an unbuffered channel, which is a rendezvous rather than a
 * queue: see the long comment at the top of chan.c. Any other capacity gives
 * a ring buffer of that many elements.
 */
gth_chan_t *gth_chan_create(size_t capacity, size_t elem_size);
void        gth_chan_destroy(gth_chan_t *c);

/* 0 on success, -1 with errno EPIPE if the channel is closed. */
int gth_chan_send(gth_chan_t *c, const void *elem);
/* 1 if a value was received, 0 if the channel is closed and drained. */
int gth_chan_recv(gth_chan_t *c, void *elem);

/* Same, but never block: -1 with errno EAGAIN instead. */
int gth_chan_trysend(gth_chan_t *c, const void *elem);
int gth_chan_tryrecv(gth_chan_t *c, void *elem);

/*
 * After close, receivers drain what is buffered and then return 0 forever,
 * every currently blocked receiver is woken, and any send is an error.
 */
void gth_chan_close(gth_chan_t *c);

size_t gth_chan_len(const gth_chan_t *c);

/* ------------------------------------------------------------------ */
/* Select                                                             */
/* ------------------------------------------------------------------ */

#define GTH_CASE_SEND 0
#define GTH_CASE_RECV 1

/*
 * Bounded because gth_select() puts its waiter nodes on the caller's stack.
 * A select over more than 16 channels is a design smell anyway.
 */
#define GTH_SELECT_MAX 16

typedef struct gth_case {
    gth_chan_t *ch;
    int         op;  /* GTH_CASE_SEND or GTH_CASE_RECV */
    void       *val; /* send: the value to send; recv: where to put it */
    int         ok;  /* out: 1 if a value moved, 0 if the channel was closed */
} gth_case_t;

/*
 * Block until one of the cases can proceed, complete exactly that one, and
 * return its index. Returns -1 with EINVAL on a malformed case array.
 */
int gth_select(gth_case_t *cases, int n);

#endif /* GTHREAD_SYNC_H */
