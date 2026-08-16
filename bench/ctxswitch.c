/*
 * ctxswitch.c - what a context switch costs, three ways.
 *
 * The identical workload runs three times: two threads passing a token back
 * and forth N times, with nothing else happening. What changes between the
 * three is only the mechanism used to hand over.
 *
 *   green   two gthread threads over an unbuffered channel
 *   condvar two pthreads over a mutex and a condition variable
 *   pipe    two pthreads over a pair of pipes, one byte each way
 *
 * Everything else is held constant: same iteration count, same -O2 build, same
 * single pinned CPU. The pthread cases are pinned to the same one core on
 * purpose - two pthreads on two cores are not context switching at all, they
 * are running side by side, and comparing that against a green thread switch
 * would be comparing two different things and calling it a speedup.
 *
 * One iteration is one round trip, which is two handovers: A to B and B back
 * to A. The number reported is per handover. For gthread a handover is two
 * swapcontext calls, because the path is thread -> scheduler -> thread; that
 * is a property of the design, not an accounting trick, and it is included.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bench_util.h"
#include "gthread.h"
#include "sync.h"

#ifndef ITERS
#define ITERS 1000000
#endif

/* ------------------------------------------------------------------ */
/* green threads                                                      */
/* ------------------------------------------------------------------ */

static gth_chan_t *to_b, *to_a;

static void *green_pong(void *arg)
{
    long i, n = (long)arg;
    int  v;

    for (i = 0; i < n; i++) {
        if (gth_chan_recv(to_b, &v) != 1)
            return (void *)1;
        if (gth_chan_send(to_a, &v) != 0)
            return (void *)1;
    }
    return NULL;
}

static double green_run(long iters)
{
    double t0, t1;
    long   i;
    int    tid, v = 1;

    tid = go_spawn(green_pong, (void *)iters);
    if (tid < 0) {
        perror("go_spawn");
        exit(1);
    }

    t0 = bench_now_ns();
    for (i = 0; i < iters; i++) {
        gth_chan_send(to_b, &v);
        gth_chan_recv(to_a, &v);
    }
    t1 = bench_now_ns();

    gth_join(tid, NULL);
    return t1 - t0;
}

/* ------------------------------------------------------------------ */
/* pthreads over a condition variable                                 */
/* ------------------------------------------------------------------ */

struct cv_state {
    pthread_mutex_t m;
    pthread_cond_t  c;
    int             turn; /* 0 = A's move, 1 = B's move */
    long            iters;
};

static void *cv_pong(void *arg)
{
    struct cv_state *s = arg;
    long i;

    for (i = 0; i < s->iters; i++) {
        pthread_mutex_lock(&s->m);
        while (s->turn != 1)
            pthread_cond_wait(&s->c, &s->m);
        s->turn = 0;
        pthread_cond_signal(&s->c);
        pthread_mutex_unlock(&s->m);
    }
    return NULL;
}

static double cv_run(long iters)
{
    struct cv_state s;
    pthread_t       th;
    double          t0, t1;
    long            i;

    pthread_mutex_init(&s.m, NULL);
    pthread_cond_init(&s.c, NULL);
    s.turn  = 0;
    s.iters = iters;

    if (pthread_create(&th, NULL, cv_pong, &s) != 0) {
        perror("pthread_create");
        exit(1);
    }

    t0 = bench_now_ns();
    for (i = 0; i < iters; i++) {
        pthread_mutex_lock(&s.m);
        s.turn = 1;
        pthread_cond_signal(&s.c);
        while (s.turn != 0)
            pthread_cond_wait(&s.c, &s.m);
        pthread_mutex_unlock(&s.m);
    }
    t1 = bench_now_ns();

    pthread_join(th, NULL);
    pthread_mutex_destroy(&s.m);
    pthread_cond_destroy(&s.c);
    return t1 - t0;
}

/* ------------------------------------------------------------------ */
/* pthreads over pipes                                                */
/* ------------------------------------------------------------------ */

struct pipe_state {
    int  a2b[2];
    int  b2a[2];
    long iters;
};

/* Pipes are the one place EINTR can appear here, and it is handled rather
 * than ignored so a stray signal cannot silently shorten the run. */
static ssize_t xfer(int fd, void *buf, size_t n, int writing)
{
    for (;;) {
        ssize_t r = writing ? write(fd, buf, n) : read(fd, buf, n);

        if (r >= 0)
            return r;
        if (errno != EINTR)
            return -1;
    }
}

static void *pipe_pong(void *arg)
{
    struct pipe_state *s = arg;
    char token;
    long i;

    for (i = 0; i < s->iters; i++) {
        if (xfer(s->a2b[0], &token, 1, 0) != 1)
            return (void *)1;
        if (xfer(s->b2a[1], &token, 1, 1) != 1)
            return (void *)1;
    }
    return NULL;
}

static double pipe_run(long iters)
{
    struct pipe_state s;
    pthread_t th;
    double    t0, t1;
    char      token = 'x';
    long      i;

    if (pipe(s.a2b) != 0 || pipe(s.b2a) != 0) {
        perror("pipe");
        exit(1);
    }
    s.iters = iters;
    if (pthread_create(&th, NULL, pipe_pong, &s) != 0) {
        perror("pthread_create");
        exit(1);
    }

    t0 = bench_now_ns();
    for (i = 0; i < iters; i++) {
        xfer(s.a2b[1], &token, 1, 1);
        xfer(s.b2a[0], &token, 1, 0);
    }
    t1 = bench_now_ns();

    pthread_join(th, NULL);
    close(s.a2b[0]); close(s.a2b[1]);
    close(s.b2a[0]); close(s.b2a[1]);
    return t1 - t0;
}

/* ------------------------------------------------------------------ */

struct result {
    const char *name;
    double median, lo, hi;
};

static void measure(const char *name, double (*fn)(long), long iters,
                    double overhead, struct result *out)
{
    double samples[BENCH_RUNS];
    int    i;

    for (i = 0; i < BENCH_WARMUP; i++)
        (void)fn(iters); /* discarded: first run pays the page faults */

    for (i = 0; i < BENCH_RUNS; i++) {
        double ns = fn(iters);

        /* Two handovers per iteration, minus the two clock reads. */
        samples[i] = (ns - overhead * 2.0) / (double)(iters * 2);
    }

    out->name = name;
    bench_stats(samples, BENCH_RUNS, &out->median, &out->lo, &out->hi);
    printf("  %-8s %8.1f ns per handover   (min %.1f, max %.1f)\n",
           name, out->median, out->lo, out->hi);
    fflush(stdout);
}

/*
 * With an argument, run only that mechanism. This exists for profiling: it is
 * how the strace and perf numbers in docs/EXPLAIN.md were taken, because
 * counting syscalls is only meaningful if the pthread comparisons - which are
 * nothing but syscalls - are not in the same process.
 *
 *   ./build/ctxswitch green
 */
int main(int argc, char **argv)
{
    struct result res[3];
    const char *only = (argc > 1) ? argv[1] : NULL;
    double overhead;
    FILE  *md;
    long   iters = ITERS;
    int    i;

    if (bench_pin_cpu(0) != 0)
        perror("warning: sched_setaffinity");

    printf("context switch cost\n");
    bench_print_machine(stdout);
    overhead = bench_timer_overhead_ns();
    printf("clock_gettime overhead: %.1f ns per call (subtracted)\n", overhead);
    printf("%ld round trips per run, %d runs, median reported\n\n",
           iters, BENCH_RUNS);

    if (gth_init() != 0) {
        perror("gth_init");
        return 1;
    }
    to_b = gth_chan_create(0, sizeof(int));
    to_a = gth_chan_create(0, sizeof(int));
    if (to_b == NULL || to_a == NULL) {
        perror("gth_chan_create");
        return 1;
    }

    measure("green", green_run, iters, overhead, &res[0]);
    if (only != NULL) {
        gth_chan_destroy(to_b);
        gth_chan_destroy(to_a);
        gth_shutdown();
        return 0;
    }
    measure("condvar", cv_run,    iters, overhead, &res[1]);
    measure("pipe",    pipe_run,  iters, overhead, &res[2]);

    printf("\n| mechanism | ns/handover (median) | min | max | vs green |\n");
    printf("|---|---|---|---|---|\n");
    for (i = 0; i < 3; i++)
        printf("| %s | %.1f | %.1f | %.1f | %.2fx |\n", res[i].name,
               res[i].median, res[i].lo, res[i].hi,
               res[i].median / res[0].median);

    md = bench_results_open();
    fprintf(md, "## Context switch cost\n\n");
    fprintf(md, "```\n");
    bench_print_machine(md);
    fprintf(md, "clock_gettime overhead: %.1f ns per call (subtracted)\n",
            overhead);
    fprintf(md, "%ld round trips per run, %d runs\n", iters, BENCH_RUNS);
    fprintf(md, "```\n\n");
    fprintf(md, "One handover is one thread giving the CPU to the other; a "
                "round trip is two.\n\n");
    fprintf(md, "| mechanism | ns/handover (median) | min | max | vs green |\n");
    fprintf(md, "|---|---|---|---|---|\n");
    for (i = 0; i < 3; i++)
        fprintf(md, "| %s | %.1f | %.1f | %.1f | %.2fx |\n", res[i].name,
                res[i].median, res[i].lo, res[i].hi,
                res[i].median / res[0].median);
    fprintf(md, "\n");
    fclose(md);

    gth_chan_destroy(to_b);
    gth_chan_destroy(to_a);
    gth_shutdown();
    return 0;
}
