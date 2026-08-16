/*
 * gthread.h - N:1 user level threading.
 *
 * Every thread created through this header runs on the same OS thread. The
 * kernel sees one thread of execution; the scheduling between the green
 * threads happens entirely in this process.
 */
#ifndef GTHREAD_H
#define GTHREAD_H

#include <stddef.h>
#include <stdint.h>

/*
 * 32 priority levels because the ready bitmap is a uint32_t and the scheduler
 * picks the highest set bit with a single instruction. See gth_sched_pick().
 * Higher number means more urgent.
 */
#define GTH_PRIO_LEVELS  32
#define GTH_PRIO_MIN     0
#define GTH_PRIO_MAX     (GTH_PRIO_LEVELS - 1)
#define GTH_PRIO_DEFAULT 16

typedef struct gth_config {
    size_t max_threads; /* size of the thread slot pool, 0 = default */
    size_t stack_size;  /* usable bytes per green stack, 0 = default */
} gth_config_t;

/*
 * gth_init() must be called once, from the process's initial thread, before
 * any other call here. It turns the caller into green thread 0, so main() can
 * spawn, yield and join like any other green thread.
 */
int  gth_init(void);
int  gth_init_config(const gth_config_t *cfg);
void gth_shutdown(void);

/*
 * go_spawn() returns a thread id, or -1 with errno set. The thread runs at
 * GTH_PRIO_DEFAULT. A joinable thread stays as a zombie slot after it returns
 * until someone calls gth_join(); a detached one releases its slot at once.
 */
int  go_spawn(void *(*fn)(void *), void *arg);
int  go_spawn_detached(void *(*fn)(void *), void *arg);
int  go_spawn_ex(void *(*fn)(void *), void *arg, int prio, int detached);

void gth_yield(void);
void gth_exit(void *retval);
int  gth_join(int tid, void **retval);

int  gth_self(void);
int  gth_getprio(void);
int  gth_setprio(int prio);

#endif /* GTHREAD_H */
