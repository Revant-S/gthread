/*
 * spawn.c - what it costs to create a thread and wait for it to finish.
 *
 * 100,000 threads, created and joined one at a time, green against pthread.
 * The thread body does nothing, so what is being measured is creation, the
 * switch into the new thread, its exit, and the join.
 *
 * Serially rather than all at once, and that is the fair shape: it is the same
 * work on both sides, and it does not run into the pthread case's real
 * constraint, which is memory rather than speed (bench/scale.c measures that
 * separately). It does mean the number includes two context switches, on both
 * sides equally.
 *
 * The gap comes from where the memory comes from. pthread_create asks the
 * kernel for a stack (mmap), asks it for a task (clone), and tears both down
 * again on join. go_spawn takes a slot off a free list and fills in a
 * ucontext: no syscall at all in the common case, because gth_init() already
 * reserved every stack in the pool.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench_util.h"
#include "gthread.h"

#ifndef NTHREADS
#define NTHREADS 100000
#endif

static volatile long sink;

static void *green_body(void *arg)
{
    sink += (long)arg;
    return NULL;
}

static void *pthread_body(void *arg)
{
    sink += (long)arg;
    return NULL;
}

static double green_run(long n)
{
    double t0, t1;
    long   i;

    t0 = bench_now_ns();
    for (i = 0; i < n; i++) {
        int tid = go_spawn(green_body, (void *)1);

        if (tid < 0) {
            perror("go_spawn");
            exit(1);
        }
        gth_join(tid, NULL);
    }
    t1 = bench_now_ns();
    return t1 - t0;
}

static double pthread_run(long n)
{
    double t0, t1;
    long   i;

    t0 = bench_now_ns();
    for (i = 0; i < n; i++) {
        pthread_t th;

        if (pthread_create(&th, NULL, pthread_body, (void *)1) != 0) {
            perror("pthread_create");
            exit(1);
        }
        pthread_join(th, NULL);
    }
    t1 = bench_now_ns();
    return t1 - t0;
}

struct result {
    const char *name;
    double median, lo, hi;
};

static void measure(const char *name, double (*fn)(long), long n,
                    double overhead, struct result *out)
{
    double samples[BENCH_RUNS];
    int    i;

    for (i = 0; i < BENCH_WARMUP; i++)
        (void)fn(n);

    for (i = 0; i < BENCH_RUNS; i++)
        samples[i] = (fn(n) - overhead * 2.0) / (double)n / 1000.0; /* us */

    out->name = name;
    bench_stats(samples, BENCH_RUNS, &out->median, &out->lo, &out->hi);
    printf("  %-8s %8.3f us per create+join   (min %.3f, max %.3f)\n",
           name, out->median, out->lo, out->hi);
    fflush(stdout);
}

int main(void)
{
    struct result res[2];
    double overhead;
    FILE  *md;
    long   n = NTHREADS;
    int    i;

    if (bench_pin_cpu(0) != 0)
        perror("warning: sched_setaffinity");

    printf("\nthread creation cost\n");
    bench_print_machine(stdout);
    overhead = bench_timer_overhead_ns();
    printf("clock_gettime overhead: %.1f ns per call (subtracted)\n", overhead);
    printf("%ld create+join pairs per run, %d runs, median reported\n\n",
           n, BENCH_RUNS);

    if (gth_init() != 0) {
        perror("gth_init");
        return 1;
    }

    measure("green",   green_run,   n, overhead, &res[0]);
    measure("pthread", pthread_run, n, overhead, &res[1]);

    printf("\n| mechanism | us per thread (median) | min | max | vs green |\n");
    printf("|---|---|---|---|---|\n");
    for (i = 0; i < 2; i++)
        printf("| %s | %.3f | %.3f | %.3f | %.1fx |\n", res[i].name,
               res[i].median, res[i].lo, res[i].hi,
               res[i].median / res[0].median);

    md = bench_results_open();
    fprintf(md, "## Thread creation cost\n\n");
    fprintf(md, "```\n");
    bench_print_machine(md);
    fprintf(md, "%ld create+join pairs per run, %d runs\n", n, BENCH_RUNS);
    fprintf(md, "```\n\n");
    fprintf(md, "| mechanism | us per thread (median) | min | max | vs green |\n");
    fprintf(md, "|---|---|---|---|---|\n");
    for (i = 0; i < 2; i++)
        fprintf(md, "| %s | %.3f | %.3f | %.3f | %.1fx |\n", res[i].name,
                res[i].median, res[i].lo, res[i].hi,
                res[i].median / res[0].median);
    fprintf(md, "\n");
    fclose(md);

    gth_shutdown();
    return 0;
}
