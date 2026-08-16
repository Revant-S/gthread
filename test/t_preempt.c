/*
 * M2 acceptance, two properties.
 *
 * 1. Three threads in tight loops that never call gth_yield() still take
 *    turns. Nothing in the loop bodies hands over control, so every handover
 *    in the transition log is one the SIGALRM handler forced.
 *
 * 2. A high priority thread that sleeps and wakes gets the CPU back from a
 *    spinning low priority thread within one tick, every time. This is the
 *    part that distinguishes fixed priority preemptive scheduling from round
 *    robin: the spinner is runnable and would have kept running.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "gthread.h"

#define NSPIN 3
#define LOGCAP 4096

static volatile int      stop_spinning;
static volatile unsigned long spins[NSPIN];

/*
 * Transition log. Only one green thread runs at a time, so the only way two
 * writers can interleave here is a tick landing between the read of log_n and
 * the store. That can drop an entry; it cannot run off the end, because the
 * bound is rechecked and the array has slack. The test only asserts a lower
 * bound on the number of handovers, so a dropped entry is harmless.
 */
static int log_ids[LOGCAP + 16];
static volatile int log_n;
static volatile int log_last = -1;

static void *spinner(void *arg)
{
    int me = (int)(long)arg;

    while (!stop_spinning) {
        if (log_last != me && log_n < LOGCAP) {
            log_last = me;
            log_ids[log_n] = me;
            log_n++;
        }
        spins[me]++;
    }
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void *sleeper(void *arg)
{
    int   rounds = (int)(long)arg;
    int   i;
    long  worst = 0;

    for (i = 0; i < rounds; i++) {
        uint32_t before = gth_ticks();
        uint32_t after;
        long late;

        gth_sleep(30);
        after = gth_ticks();
        /* 30 ms is 3 ticks; anything beyond that is scheduling latency. */
        late = (long)(after - before) - 3;
        if (late > worst)
            worst = late;
    }
    return (void *)worst;
}

static int check_preemption(void)
{
    int  tid[NSPIN];
    int  i, seen[NSPIN] = {0};
    double t0;

    for (i = 0; i < NSPIN; i++) {
        tid[i] = go_spawn(spinner, (void *)(long)i);
        if (tid[i] < 0) {
            perror("go_spawn");
            return 1;
        }
    }

    /* Let them fight over the CPU for a while. main sleeps, so it is out of
     * the way and the three spinners are the only runnable threads. */
    t0 = now_ms();
    gth_sleep(300);
    printf("spun for %.1f ms\n", now_ms() - t0);

    stop_spinning = 1;
    for (i = 0; i < NSPIN; i++)
        gth_join(tid[i], NULL);

    for (i = 0; i < log_n; i++)
        seen[log_ids[i]] = 1;

    printf("handovers observed: %d\n", log_n);
    printf("first 12 of the transition log:");
    for (i = 0; i < log_n && i < 12; i++)
        printf(" %d", log_ids[i]);
    printf("\n");
    for (i = 0; i < NSPIN; i++)
        printf("thread %d: %lu iterations\n", i, spins[i]);

    for (i = 0; i < NSPIN; i++) {
        if (!seen[i] || spins[i] == 0) {
            fprintf(stderr, "FAIL: thread %d never ran\n", i);
            return 1;
        }
    }
    /* 300 ms at a 10 ms tick is ~30 forced switches; require most of them. */
    if (log_n < 20) {
        fprintf(stderr, "FAIL: only %d handovers, expected ~30\n", log_n);
        return 1;
    }
    return 0;
}

static int check_priority(void)
{
    int  spin_tid, sleep_tid;
    void *rv = NULL;
    long worst;

    stop_spinning = 0;
    spin_tid = go_spawn_ex(spinner, (void *)(long)0, GTH_PRIO_DEFAULT - 4, 0);
    sleep_tid = go_spawn_ex(sleeper, (void *)(long)10, GTH_PRIO_DEFAULT + 4, 0);
    if (spin_tid < 0 || sleep_tid < 0) {
        perror("go_spawn_ex");
        return 1;
    }

    gth_join(sleep_tid, &rv);
    stop_spinning = 1;
    gth_join(spin_tid, NULL);

    worst = (long)rv;
    printf("worst wake latency over 10 sleeps, against a spinning "
           "lower priority thread: %ld tick(s)\n", worst);
    if (worst > 1) {
        fprintf(stderr, "FAIL: high priority thread waited %ld ticks\n", worst);
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
    if (check_preemption() != 0)
        return 1;
    if (check_priority() != 0)
        return 1;

    printf("PASS: threads that never yield are preempted, and priority is "
           "honoured on wakeup\n");
    gth_shutdown();
    return 0;
}
