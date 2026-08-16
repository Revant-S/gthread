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

/*
 * Sleep for at least ms milliseconds. The resolution is the scheduler tick
 * (10 ms), and the deadline is rounded up, so gth_sleep(1) parks until the
 * next tick rather than returning immediately.
 */
void     gth_sleep(unsigned ms);
uint32_t gth_ticks(void);

int  gth_self(void);
int  gth_getprio(void);
int  gth_setprio(int prio);

/* ------------------------------------------------------------------ */
/* Netpoller: blocking-looking I/O that does not block the process     */
/* ------------------------------------------------------------------ */

#include <sys/socket.h>
#include <sys/types.h>

/*
 * These look like read(2), write(2), accept(2) and connect(2) and behave like
 * them, except that when the fd is not ready they park the calling green
 * thread and let the rest of the runtime carry on. The fd must be
 * O_NONBLOCK - gth_socket() and gth_accept() take care of that, or call
 * gth_set_nonblocking() on an fd from elsewhere.
 */
int     gth_set_nonblocking(int fd);
int     gth_socket(int domain, int type, int protocol);
ssize_t gth_read(int fd, void *buf, size_t count);
ssize_t gth_write(int fd, const void *buf, size_t count);
int     gth_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int     gth_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int     gth_close(int fd);

/* Write all of it, or fail. Short writes on a socket are the normal case. */
ssize_t gth_write_all(int fd, const void *buf, size_t count);

#endif /* GTHREAD_H */
