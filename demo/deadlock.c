/*
 * deadlock.c - the same two threads and two mutexes, built two ways.
 *
 *   make deadlock   ->  -DDEADLOCK_DEMO, the threads take the locks in
 *                       opposite orders, and the program stops dead.
 *   make demos      ->  both threads take the locks in one global order, and
 *                       the program runs until you stop it.
 *
 * The interesting part is that the deadlocked build does not crash, does not
 * spin and does not burn a core. It reaches a state where no green thread is
 * runnable and none of them can ever be made runnable by another, the
 * scheduler notices that and says so, and the process sits there. That is
 * what a deadlock looks like from inside the runtime, and it is worth seeing
 * once: the machine is fine, the program is simply never going to make
 * progress again.
 *
 * The fix in the non-demo build is the one that always works and never needs
 * a deadlock detector: give the locks a global rank and require that they are
 * acquired in increasing rank order. If every thread does that, a cycle in
 * the wait-for graph is impossible, because a cycle needs at least one thread
 * holding a higher ranked lock while waiting for a lower ranked one.
 */
#include <stdio.h>
#include <stdlib.h>

#include "gthread.h"
#include "sync.h"

static gth_mutex_t lock_a; /* rank 0 */
static gth_mutex_t lock_b; /* rank 1 */

static volatile long rounds_a, rounds_b;
static long limit; /* 0 means run forever */

static void critical_work(const char *who, volatile long *counter)
{
    (*counter)++;
    if (*counter % 100000 == 0)
        printf("%s: %ld rounds\n", who, *counter);
}

static void *thread_a(void *arg)
{
    (void)arg;
    while (limit == 0 || rounds_a < limit) {
        gth_mutex_lock(&lock_a);
        gth_mutex_lock(&lock_b);
        critical_work("thread A", &rounds_a);
        gth_mutex_unlock(&lock_b);
        gth_mutex_unlock(&lock_a);
    }
    return NULL;
}

static void *thread_b(void *arg)
{
    (void)arg;
    while (limit == 0 || rounds_b < limit) {
#ifdef DEADLOCK_DEMO
        /*
         * Reversed. Thread A can hold lock_a and want lock_b at the same
         * moment that thread B holds lock_b and wants lock_a, and neither
         * will ever let go. The window is small enough that this build often
         * runs for a few thousand rounds before it hits it, which is exactly
         * why deadlocks like this survive testing.
         */
        gth_mutex_lock(&lock_b);
        gth_mutex_lock(&lock_a);
#else
        gth_mutex_lock(&lock_a);
        gth_mutex_lock(&lock_b);
#endif
        critical_work("thread B", &rounds_b);
#ifdef DEADLOCK_DEMO
        gth_mutex_unlock(&lock_a);
        gth_mutex_unlock(&lock_b);
#else
        gth_mutex_unlock(&lock_b);
        gth_mutex_unlock(&lock_a);
#endif
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int ta, tb;

    if (argc > 1)
        limit = strtol(argv[1], NULL, 10);

    if (gth_init() != 0) {
        perror("gth_init");
        return 1;
    }
    gth_mutex_init(&lock_a);
    gth_mutex_init(&lock_b);

#ifdef DEADLOCK_DEMO
    puts("built with -DDEADLOCK_DEMO: the two threads take the locks in "
         "opposite orders.");
    puts("expect it to stop making progress. ctrl-c to quit.");
#else
    puts("locks are taken in rank order (a before b) by both threads.");
    if (limit == 0)
        puts("this build runs until you stop it. ctrl-c to quit.");
#endif

    ta = go_spawn(thread_a, NULL);
    tb = go_spawn(thread_b, NULL);
    if (ta < 0 || tb < 0) {
        perror("go_spawn");
        return 1;
    }
    gth_join(ta, NULL);
    gth_join(tb, NULL);

    printf("done: thread A %ld rounds, thread B %ld rounds\n",
           rounds_a, rounds_b);
    gth_shutdown();
    return 0;
}
