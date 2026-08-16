/*
 * M3 acceptance.
 *
 * 1. Producer/consumer over a semaphore-guarded ring buffer, with sequence
 *    numbers checked on the way out: nothing lost, nothing duplicated,
 *    nothing reordered within a producer.
 * 2. Recursive locking is reported rather than silently allowed.
 * 3. Priority inheritance actually shortens the inversion. The test builds
 *    the classic low/medium/high arrangement and measures how long the high
 *    priority thread waits. Without inheritance it would wait for the medium
 *    priority spinner to finish, which is roughly twice as long.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gthread.h"
#include "sync.h"

/* ---------------- producer / consumer ---------------- */

#define RING     16
#define NPROD    2
#define PER_PROD 5000

struct item {
    int producer;
    int seq;
};

static struct item ring[RING];
static int         ring_in, ring_out;
static gth_sem_t   slots_free, slots_used;
static gth_mutex_t ring_lock;

static void *producer(void *arg)
{
    int me = (int)(long)arg;
    int i;

    for (i = 0; i < PER_PROD; i++) {
        struct item it = { me, i };

        gth_sem_wait(&slots_free);
        gth_mutex_lock(&ring_lock);
        ring[ring_in] = it;
        ring_in = (ring_in + 1) % RING;
        gth_mutex_unlock(&ring_lock);
        gth_sem_post(&slots_used);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    long  expect[NPROD];
    long  total = NPROD * (long)PER_PROD;
    long  n;
    int   i;

    (void)arg;
    for (i = 0; i < NPROD; i++)
        expect[i] = 0;

    for (n = 0; n < total; n++) {
        struct item it;

        gth_sem_wait(&slots_used);
        gth_mutex_lock(&ring_lock);
        it = ring[ring_out];
        ring_out = (ring_out + 1) % RING;
        gth_mutex_unlock(&ring_lock);
        gth_sem_post(&slots_free);

        if (it.producer < 0 || it.producer >= NPROD ||
            it.seq != expect[it.producer]) {
            fprintf(stderr, "FAIL: item %ld out of order: producer %d seq %d, "
                    "expected %ld\n", n, it.producer, it.seq,
                    it.producer >= 0 && it.producer < NPROD
                        ? expect[it.producer] : -1L);
            return (void *)1;
        }
        expect[it.producer]++;
    }
    return NULL;
}

static int check_prodcons(void)
{
    int  ptid[NPROD], ctid;
    void *rv = NULL;
    int  i;

    gth_sem_init(&slots_free, RING);
    gth_sem_init(&slots_used, 0);
    gth_mutex_init(&ring_lock);
    ring_in = ring_out = 0;

    ctid = go_spawn(consumer, NULL);
    for (i = 0; i < NPROD; i++)
        ptid[i] = go_spawn(producer, (void *)(long)i);

    for (i = 0; i < NPROD; i++)
        gth_join(ptid[i], NULL);
    gth_join(ctid, &rv);

    if (rv != NULL)
        return 1;
    printf("producer/consumer: %d items through a %d slot ring, all in "
           "order\n", NPROD * PER_PROD, RING);
    return 0;
}

/* ---------------- recursive lock ---------------- */

static int check_recursive(void)
{
    gth_mutex_t m;

    gth_mutex_init(&m);
    if (gth_mutex_lock(&m) != 0) {
        fprintf(stderr, "FAIL: first lock failed\n");
        return 1;
    }
    if (gth_mutex_lock(&m) == 0 || errno != EDEADLK) {
        fprintf(stderr, "FAIL: recursive lock was allowed\n");
        return 1;
    }
    gth_mutex_unlock(&m);
    if (gth_mutex_unlock(&m) == 0 || errno != EPERM) {
        fprintf(stderr, "FAIL: unlock of an unheld mutex was allowed\n");
        return 1;
    }
    printf("recursive lock rejected with EDEADLK, stray unlock with EPERM\n");
    return 0;
}

/* ---------------- priority inheritance ---------------- */

static gth_mutex_t pi_lock;
static double      pi_wait_ms;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Burn CPU without yielding, so the only way off this thread is preemption. */
static void spin_ms(double ms)
{
    double end = now_ms() + ms;
    while (now_ms() < end)
        ;
}

static void *pi_low(void *arg)
{
    (void)arg;
    gth_mutex_lock(&pi_lock);
    spin_ms(100);
    gth_mutex_unlock(&pi_lock);
    return NULL;
}

static void *pi_medium(void *arg)
{
    (void)arg;
    gth_sleep(20);
    spin_ms(150);
    return NULL;
}

static void *pi_high(void *arg)
{
    double t0;

    (void)arg;
    gth_sleep(40);
    t0 = now_ms();
    gth_mutex_lock(&pi_lock);
    pi_wait_ms = now_ms() - t0;
    gth_mutex_unlock(&pi_lock);
    return NULL;
}

static int check_priority_inheritance(void)
{
    int lo, md, hi;

    gth_mutex_init(&pi_lock);
    pi_wait_ms = 0;

    /*
     * The sleeps stagger the start so the arrangement is the textbook one:
     * low takes the lock at t=0, medium starts spinning at t=20, high blocks
     * on the lock at t=40. Low has ~60 ms of work left at that point.
     */
    lo = go_spawn_ex(pi_low, NULL, GTH_PRIO_DEFAULT - 8, 0);
    md = go_spawn_ex(pi_medium, NULL, GTH_PRIO_DEFAULT, 0);
    hi = go_spawn_ex(pi_high, NULL, GTH_PRIO_DEFAULT + 8, 0);
    if (lo < 0 || md < 0 || hi < 0) {
        perror("go_spawn_ex");
        return 1;
    }
    gth_join(hi, NULL);
    gth_join(md, NULL);
    gth_join(lo, NULL);

    printf("priority inheritance: high priority thread waited %.1f ms for a "
           "lock held by a low priority thread\n", pi_wait_ms);
    printf("  (without inheritance it would have had to wait for the medium "
           "priority spinner to finish first)\n");
    if (pi_wait_ms > 150.0) {
        fprintf(stderr, "FAIL: inversion was not resolved (%.1f ms)\n",
                pi_wait_ms);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (gth_init() != 0) {
        perror("gth_init");
        return 1;
    }
    if (check_prodcons() != 0)
        return 1;
    if (check_recursive() != 0)
        return 1;
    if (check_priority_inheritance() != 0)
        return 1;

    printf("PASS: semaphore, mutex and priority inheritance behave\n");
    gth_shutdown();
    return 0;
}
