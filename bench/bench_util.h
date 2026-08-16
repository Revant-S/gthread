/*
 * bench_util.h - the parts every benchmark needs: timing, pinning, statistics,
 * and the machine description.
 *
 * The rules these helpers enforce are not decoration. A context switch number
 * without the CPU model and the frequency governor next to it is not a result,
 * it is a rumour, and a single run of a microbenchmark says as much about what
 * else the machine was doing as about the code.
 */
#ifndef BENCH_UTIL_H
#define BENCH_UTIL_H

#include <gnu/libc-version.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define BENCH_RUNS   11 /* median of 11, plus min and max, never a single run */
#define BENCH_WARMUP 1  /* discarded: first run pays for page faults and icache */

static inline double bench_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/*
 * The cost of asking what time it is, so it can be subtracted from what is
 * being measured. On a machine where clock_gettime is a vDSO call this is
 * around 20 ns; on one where it is a real syscall it is ten times that and it
 * would otherwise be a visible part of a sub-microsecond measurement.
 */
static inline double bench_timer_overhead_ns(void)
{
    const int n = 200000;
    double t0, t1;
    int i;

    t0 = bench_now_ns();
    for (i = 0; i < n; i++)
        (void)bench_now_ns();
    t1 = bench_now_ns();
    return (t1 - t0) / n;
}

/*
 * Pin to one CPU. Without this the scheduler is free to migrate the process
 * between cores between runs, and a migration costs cache and shows up as a
 * 30% outlier in a benchmark that is measuring hundreds of nanoseconds. It
 * also makes the pthread comparison honest: both sides contend for exactly one
 * core, which is the situation where a context switch is actually a context
 * switch rather than two threads running side by side.
 */
static inline int bench_pin_cpu(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

static inline void bench_read_first_line(const char *path, const char *prefix,
                                  char *out, size_t outlen)
{
    FILE *f = fopen(path, "r");
    char  line[256];

    snprintf(out, outlen, "unknown");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *nl;

        if (prefix != NULL && strncmp(line, prefix, strlen(prefix)) != 0)
            continue;
        if (prefix != NULL) {
            char *colon = strchr(line, ':');
            if (colon != NULL) {
                char *v = colon + 1;
                while (*v == ' ' || *v == '\t')
                    v++;
                snprintf(out, outlen, "%s", v);
            }
        } else {
            snprintf(out, outlen, "%s", line);
        }
        nl = strchr(out, '\n');
        if (nl != NULL)
            *nl = '\0';
        break;
    }
    fclose(f);
}

static inline void bench_print_machine(FILE *f)
{
    char cpu[256], gov[64];
    struct utsname un;
    time_t now = time(NULL);
    struct tm tm;

    bench_read_first_line("/proc/cpuinfo", "model name", cpu, sizeof(cpu));
    bench_read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
                          NULL, gov, sizeof(gov));
    uname(&un);
    gmtime_r(&now, &tm);

    fprintf(f, "machine:  %s\n", cpu);
    fprintf(f, "cores:    %ld online\n", sysconf(_SC_NPROCESSORS_ONLN));
    fprintf(f, "kernel:   %s %s\n", un.sysname, un.release);
    fprintf(f, "glibc:    %s\n", gnu_get_libc_version());
    fprintf(f, "compiler: %s\n", __VERSION__);
    fprintf(f, "governor: %s\n", gov);
    fprintf(f, "measured: %04d-%02d-%02d UTC\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    fprintf(f, "build:    benchmarks and the library they link are both -O2; "
               "the library is -Og everywhere else\n");
    fprintf(f, "affinity: pinned to CPU 0 with sched_setaffinity\n");
}

static inline int bench_cmp(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    return (x > y) - (x < y);
}

/*
 * Median, min, max. The median rather than the mean because a benchmark's
 * outliers are one sided - something else got scheduled, an interrupt landed -
 * so the mean drifts upwards with the noise while the median does not. The
 * min and max are printed alongside because a median with a 10x max next to it
 * is a different result from a median with a 1.05x max, and hiding that would
 * be dishonest.
 */
static inline void bench_stats(double *v, int n, double *median, double *lo, double *hi)
{
    qsort(v, (size_t)n, sizeof(*v), bench_cmp);
    *lo     = v[0];
    *hi     = v[n - 1];
    *median = (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/*
 * results.md is opened in append mode: `make bench` deletes it first and then
 * each benchmark adds its own section, so the file is always exactly the set
 * of numbers from one complete run on one machine.
 */
static inline FILE *bench_results_open(void)
{
    const char *path = getenv("GTH_RESULTS");
    FILE *f;

    if (path == NULL)
        path = "bench/results.md";
    f = fopen(path, "a");
    if (f == NULL) {
        perror("bench: cannot open results file");
        exit(1);
    }
    return f;
}

/* Resident and virtual size in KiB, straight from the kernel. */
static inline void bench_mem_kb(long *rss_kb, long *vsz_kb)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long  total_pages = 0, resident_pages = 0;
    long  page_kb = sysconf(_SC_PAGESIZE) / 1024;

    *rss_kb = *vsz_kb = 0;
    if (f == NULL)
        return;
    if (fscanf(f, "%ld %ld", &total_pages, &resident_pages) == 2) {
        *vsz_kb = total_pages * page_kb;
        *rss_kb = resident_pages * page_kb;
    }
    fclose(f);
}

#endif /* BENCH_UTIL_H */
