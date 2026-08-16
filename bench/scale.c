/*
 * scale.c - how much memory a live but idle thread costs.
 *
 * 10,000 threads that have started, touched their stacks and then parked. The
 * measurement is the change in resident set size across creating them, read
 * from /proc/self/statm, divided by the number of threads.
 *
 * Resident, not virtual, is the number that matters, and the difference
 * between the two is the whole story of this benchmark. A pthread reserves
 * 8 MiB of address space for its stack by default - the value is printed below
 * from pthread_attr_getstacksize, because without it the comparison looks
 * arbitrary - but it only faults in the pages it actually touches. A green
 * thread here reserves 32 KiB and touches one or two pages of it. So the
 * virtual footprints differ by 250x and the resident footprints by far less;
 * quoting the virtual number as if it were memory would be the dishonest
 * version of this result.
 *
 * Each side runs in a forked child, so neither measurement is polluted by the
 * other's allocations, and so that a pthread case that runs out of memory
 * takes only itself down.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bench_util.h"
#include "gthread.h"
#include "sync.h"

#ifndef NTHREADS
#define NTHREADS 10000
#endif

#define GREEN_STACK (32 * 1024)

struct report {
    long   created;
    double rss_per_thread_kb;
    double vsz_per_thread_kb;
    long   rss_before_kb, rss_after_kb;
    long   vsz_before_kb, vsz_after_kb;
    int    failed_at;      /* thread number where creation failed, 0 if none */
    int    fail_errno;
};

/* ------------------------------------------------------------------ */
/* green                                                              */
/* ------------------------------------------------------------------ */

static gth_chan_t   *park;
static gth_mutex_t   count_lock;
static long          started;

static void *green_idle(void *arg)
{
    int v;

    (void)arg;
    /*
     * Touch a page of stack before parking, so the measurement includes what
     * a thread that has actually run costs, not what an untouched stack
     * costs. A benchmark that reported the cost of a thread that never ran
     * would be measuring nothing.
     */
    {
        volatile char pad[512];
        memset((void *)pad, 1, sizeof(pad));
    }
    gth_mutex_lock(&count_lock);
    started++;
    gth_mutex_unlock(&count_lock);

    gth_chan_recv(park, &v); /* parks forever: the channel is never written */
    return NULL;
}

static void green_measure(struct report *r)
{
    gth_config_t cfg;
    long i;

    memset(r, 0, sizeof(*r));
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_threads = NTHREADS + 16;
    cfg.stack_size  = GREEN_STACK;
    if (gth_init_config(&cfg) != 0) {
        perror("gth_init_config");
        exit(1);
    }
    gth_mutex_init(&count_lock);
    park = gth_chan_create(0, sizeof(int));

    /*
     * Measured after gth_init, so the pool's address space reservation is not
     * charged to the threads. What is being asked is "what does one more live
     * thread cost", and the answer for a pool allocator is the pages it
     * touches, with the reservation as a separate one off.
     */
    bench_mem_kb(&r->rss_before_kb, &r->vsz_before_kb);

    for (i = 0; i < NTHREADS; i++) {
        if (go_spawn_detached(green_idle, NULL) < 0) {
            r->failed_at  = (int)i + 1;
            r->fail_errno = errno;
            break;
        }
        r->created++;
    }

    /* Let every one of them run far enough to park. */
    while (started < r->created)
        gth_sleep(10);

    bench_mem_kb(&r->rss_after_kb, &r->vsz_after_kb);
    r->rss_per_thread_kb = (double)(r->rss_after_kb - r->rss_before_kb) /
                           (double)r->created;
    r->vsz_per_thread_kb = (double)(r->vsz_after_kb - r->vsz_before_kb) /
                           (double)r->created;
}

/* ------------------------------------------------------------------ */
/* pthread                                                            */
/* ------------------------------------------------------------------ */

static pthread_mutex_t pt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  pt_cond = PTHREAD_COND_INITIALIZER;
static long            pt_started;
static int             pt_release;

static void *pthread_idle(void *arg)
{
    (void)arg;
    {
        volatile char pad[512];
        memset((void *)pad, 1, sizeof(pad));
    }
    pthread_mutex_lock(&pt_lock);
    pt_started++;
    while (!pt_release)
        pthread_cond_wait(&pt_cond, &pt_lock);
    pthread_mutex_unlock(&pt_lock);
    return NULL;
}

static void pthread_measure(struct report *r)
{
    pthread_attr_t attr;
    long i;

    memset(r, 0, sizeof(*r));
    bench_mem_kb(&r->rss_before_kb, &r->vsz_before_kb);

    pthread_attr_init(&attr);
    for (i = 0; i < NTHREADS; i++) {
        pthread_t th;
        int rc = pthread_create(&th, NULL, pthread_idle, NULL);

        if (rc != 0) {
            r->failed_at  = (int)i + 1;
            r->fail_errno = rc;
            break;
        }
        pthread_detach(th);
        r->created++;
    }
    pthread_attr_destroy(&attr);

    for (;;) {
        long s;
        pthread_mutex_lock(&pt_lock);
        s = pt_started;
        pthread_mutex_unlock(&pt_lock);
        if (s >= r->created)
            break;
        usleep(10000);
    }

    bench_mem_kb(&r->rss_after_kb, &r->vsz_after_kb);
    r->rss_per_thread_kb = (double)(r->rss_after_kb - r->rss_before_kb) /
                           (double)(r->created ? r->created : 1);
    r->vsz_per_thread_kb = (double)(r->vsz_after_kb - r->vsz_before_kb) /
                           (double)(r->created ? r->created : 1);
}

/* ------------------------------------------------------------------ */

static void run_in_child(void (*fn)(struct report *), struct report *out)
{
    int   fds[2];
    pid_t pid;

    if (pipe(fds) != 0) {
        perror("pipe");
        exit(1);
    }
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        struct report r;

        close(fds[0]);
        fn(&r);
        if (write(fds[1], &r, sizeof(r)) != (ssize_t)sizeof(r))
            _exit(1);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    if (read(fds[0], out, sizeof(*out)) != (ssize_t)sizeof(*out)) {
        fprintf(stderr, "child produced no result\n");
        memset(out, 0, sizeof(*out));
    }
    close(fds[0]);
    waitpid(pid, NULL, 0);
}

static void emit(FILE *f, const struct report *g, const struct report *p,
                 size_t pthread_stack)
{
    fprintf(f, "| runtime | threads created | RSS/thread (KiB) | "
               "virtual/thread (KiB) | total RSS delta (KiB) |\n");
    fprintf(f, "|---|---|---|---|---|\n");
    fprintf(f, "| green (%d KiB stacks) | %ld | %.2f | %.1f | %ld |\n",
            GREEN_STACK / 1024, g->created, g->rss_per_thread_kb,
            g->vsz_per_thread_kb, g->rss_after_kb - g->rss_before_kb);
    fprintf(f, "| pthread (%zu KiB default stacks) | %ld | %.2f | %.1f | %ld |\n",
            pthread_stack / 1024, p->created, p->rss_per_thread_kb,
            p->vsz_per_thread_kb, p->rss_after_kb - p->rss_before_kb);
    fprintf(f, "\n");
    if (g->failed_at)
        fprintf(f, "green creation failed at thread %d: %s\n\n",
                g->failed_at, strerror(g->fail_errno));
    if (p->failed_at)
        fprintf(f, "pthread creation failed at thread %d: %s\n\n",
                p->failed_at, strerror(p->fail_errno));
    else
        fprintf(f, "pthread creation did not fail at %d threads on this "
                   "machine.\n\n", NTHREADS);
}

int main(void)
{
    struct report green, pth;
    pthread_attr_t attr;
    size_t stack = 0;
    FILE *md;

    if (bench_pin_cpu(0) != 0)
        perror("warning: sched_setaffinity");

    pthread_attr_init(&attr);
    pthread_attr_getstacksize(&attr, &stack);
    pthread_attr_destroy(&attr);

    printf("\nmemory per idle thread, %d threads\n", NTHREADS);
    bench_print_machine(stdout);
    printf("pthread default stack size: %zu KiB (pthread_attr_getstacksize)\n",
           stack / 1024);
    printf("green stack size: %d KiB\n", GREEN_STACK / 1024);
    printf("memory is read from /proc/self/statm and is deterministic, so this "
           "one runs once rather than 11 times\n\n");
    fflush(stdout);

    run_in_child(green_measure, &green);
    run_in_child(pthread_measure, &pth);

    emit(stdout, &green, &pth, stack);

    md = bench_results_open();
    fprintf(md, "## Memory per idle thread\n\n");
    fprintf(md, "```\n");
    bench_print_machine(md);
    fprintf(md, "%d idle threads per runtime, each measured in its own "
                "forked process\n", NTHREADS);
    fprintf(md, "pthread default stack size: %zu KiB\n", stack / 1024);
    fprintf(md, "green stack size: %d KiB\n", GREEN_STACK / 1024);
    fprintf(md, "```\n\n");
    emit(md, &green, &pth, stack);
    fclose(md);
    return 0;
}
