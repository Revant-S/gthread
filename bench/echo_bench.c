/*
 * echo_bench.c - the end to end number: throughput and latency of an echo
 * server at 1k, 5k and 10k concurrent connections, green threads against one
 * pthread per connection.
 *
 * Three processes worth of care went into making this comparison mean
 * something:
 *
 *  - The two servers are the same program with the concurrency swapped out.
 *    Same accept loop, same read/echo loop, same buffer size, same TCP_NODELAY,
 *    same -O2. What differs is that one spawns a green thread per connection
 *    and the other spawns a pthread.
 *
 *  - Both servers are pinned to one core. The green server has no choice - it
 *    is one OS thread - and letting the pthread server spread over twelve
 *    cores would be measuring parallelism, not concurrency, and calling the
 *    difference a win for green threads. The honest caveat is the other way
 *    round and belongs in the README: on a real machine the pthread server can
 *    use every core and this runtime cannot, which is what M:N would buy.
 *
 *  - The load generator is a plain epoll loop with no green threads in it, on
 *    a different core, so the thing being measured is the server.
 *
 * A request is 64 bytes out and 64 bytes back. Latency is measured from the
 * write to the matching read, per request, and reported as p50 and p99.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bench_util.h"
#include "gthread.h"

#define PAYLOAD      64
#define BASE_PORT    18500
#define WINDOW_SEC   1.0
#define MAX_SAMPLES  (4 * 1000 * 1000)

static const int conn_counts[] = { 1000, 5000, 10000 };
#define NCONFIGS ((int)(sizeof(conn_counts) / sizeof(conn_counts[0])))

/* ------------------------------------------------------------------ */
/* servers (run in a forked child, pinned to CPU 0)                    */
/* ------------------------------------------------------------------ */

static int listen_on(int port, int nonblocking)
{
    struct sockaddr_in addr;
    int fd, one = 1;

    fd = socket(AF_INET, SOCK_STREAM | (nonblocking ? SOCK_NONBLOCK : 0), 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 4096) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void *green_conn(void *arg)
{
    int  fd = (int)(long)arg;
    char buf[PAYLOAD];

    for (;;) {
        ssize_t n = gth_read(fd, buf, sizeof(buf));

        if (n <= 0)
            break;
        if (gth_write_all(fd, buf, (size_t)n) < 0)
            break;
    }
    gth_close(fd);
    return NULL;
}

static void green_server(int port, int maxconn)
{
    gth_config_t cfg;
    int listener;

    memset(&cfg, 0, sizeof(cfg));
    cfg.max_threads = (size_t)maxconn + 64;
    cfg.stack_size  = 32 * 1024;
    if (gth_init_config(&cfg) != 0)
        _exit(1);

    listener = listen_on(port, 1);
    if (listener < 0)
        _exit(1);

    for (;;) {
        int one = 1;
        int fd = gth_accept(listener, NULL, NULL);

        if (fd < 0)
            break;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (go_spawn_detached(green_conn, (void *)(long)fd) < 0)
            gth_close(fd);
    }
    _exit(0);
}

static void *pthread_conn(void *arg)
{
    int  fd = (int)(long)arg;
    char buf[PAYLOAD];

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        ssize_t done = 0;

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        while (done < n) {
            ssize_t w = write(fd, buf + done, (size_t)(n - done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                goto out;
            done += w;
        }
    }
out:
    close(fd);
    return NULL;
}

static void pthread_server(int port)
{
    int  listener = listen_on(port, 0);
    long accepted = 0;

    if (listener < 0)
        _exit(1);
    signal(SIGPIPE, SIG_IGN);

    for (;;) {
        pthread_t th;
        int one = 1;
        int fd = accept(listener, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            break;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        {
            int rc = pthread_create(&th, NULL, pthread_conn, (void *)(long)fd);

            if (rc != 0) {
                /*
                 * Where the pthread-per-connection model actually breaks, and
                 * the number the brief asks for. Reported rather than
                 * swallowed: a server that quietly drops connections at some
                 * unknown thread count is a worse result than one that says
                 * where the wall is.
                 */
                fprintf(stderr, "pthread server: pthread_create failed after "
                        "%ld connections: %s\n", accepted, strerror(rc));
                fflush(stderr);
                close(fd);
                continue;
            }
        }
        pthread_detach(th);
        accepted++;
    }
    _exit(0);
}

static pid_t start_server(int green, int port, int maxconn)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        bench_pin_cpu(0);
        if (green)
            green_server(port, maxconn);
        else
            pthread_server(port);
        _exit(0);
    }
    return pid;
}

/* ------------------------------------------------------------------ */
/* load generator (parent, pinned to CPU 1)                            */
/* ------------------------------------------------------------------ */

struct conn {
    int    fd;
    int    got;      /* bytes of the current reply received so far */
    int    ready;    /* the connection is established */
    double sent_at;
};

static double *samples;
static long    nsamples;

/*
 * Open every connection and, importantly, wait until every one of them is
 * actually established before any traffic starts.
 *
 * This is not paranoia. connect() on a non-blocking socket returns
 * EINPROGRESS and completes when the server's accept queue takes it; with
 * thousands of connections arriving at once, a server whose accept loop is
 * slow lets that queue overflow and the kernel drops the SYN, to be
 * retransmitted a second later. Writing to a socket that is not connected yet
 * returns EAGAIN, and an earlier version of this benchmark reported that as
 * "the server failed" - which was a bug in the measuring instrument, not a
 * result. The wait below is a full establishment phase with a generous
 * budget, so what gets measured afterwards is steady state service and not
 * connection setup.
 */
static int connect_all(struct conn *c, int n, int port, int epfd)
{
    struct sockaddr_in addr;
    struct epoll_event evs[512];
    double deadline;
    int i, ok = 0, established = 0;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((uint16_t)port);

    for (i = 0; i < n; i++) {
        struct epoll_event ev;
        int one = 1;

        c[i].fd    = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        c[i].got   = 0;
        c[i].ready = 0;
        if (c[i].fd < 0)
            return ok;
        setsockopt(c[i].fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (connect(c[i].fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 &&
            errno != EINPROGRESS) {
            close(c[i].fd);
            c[i].fd = -1;
            return ok;
        }
        memset(&ev, 0, sizeof(ev));
        ev.events   = EPOLLOUT;
        ev.data.u32 = (uint32_t)i;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, c[i].fd, &ev) != 0)
            return ok;
        ok++;
    }

    deadline = bench_now_ns() + 60e9;
    while (established < ok && bench_now_ns() < deadline) {
        int nev = epoll_wait(epfd, evs, (int)(sizeof(evs) / sizeof(evs[0])), 500);

        if (nev < 0) {
            if (errno == EINTR)
                continue;
            return established;
        }
        for (i = 0; i < nev; i++) {
            struct conn *cn = &c[evs[i].data.u32];
            struct epoll_event ev;
            int err = 0;
            socklen_t len = sizeof(err);

            if (cn->ready)
                continue;
            if (getsockopt(cn->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
                err != 0)
                continue; /* still pending, or refused: retried by the kernel */
            cn->ready = 1;
            established++;
            memset(&ev, 0, sizeof(ev));
            ev.events   = EPOLLIN;
            ev.data.u32 = evs[i].data.u32;
            epoll_ctl(epfd, EPOLL_CTL_MOD, cn->fd, &ev);
        }
    }
    return established;
}

static int send_request(struct conn *c)
{
    char buf[PAYLOAD];
    ssize_t n;

    memset(buf, 'x', sizeof(buf));
    c->got     = 0;
    c->sent_at = bench_now_ns();
    for (;;) {
        n = write(c->fd, buf, sizeof(buf));
        if (n == (ssize_t)sizeof(buf))
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        return -1; /* a short write on 64 bytes to loopback means trouble */
    }
}

/*
 * One measurement window: every connection has a request outstanding, replies
 * are collected as they arrive, and each completed reply is immediately
 * followed by the next request until the window closes.
 */
static double run_window(struct conn *c, int n, int epfd, double seconds,
                         long *requests)
{
    struct epoll_event evs[512];
    double t0 = bench_now_ns();
    double deadline = t0 + seconds * 1e9;
    long   done = 0;
    int    i;

    for (i = 0; i < n; i++) {
        if (send_request(&c[i]) != 0)
            return -1;
    }

    while (bench_now_ns() < deadline) {
        int nev = epoll_wait(epfd, evs, (int)(sizeof(evs) / sizeof(evs[0])), 100);

        if (nev < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        for (i = 0; i < nev; i++) {
            struct conn *cn = &c[evs[i].data.u32];
            char buf[PAYLOAD];
            ssize_t r = read(cn->fd, buf, (size_t)(PAYLOAD - cn->got));

            if (r < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                return -1;
            }
            if (r == 0)
                return -1; /* server closed: the run is invalid */
            cn->got += (int)r;
            if (cn->got < PAYLOAD)
                continue;

            if (nsamples < MAX_SAMPLES)
                samples[nsamples++] = bench_now_ns() - cn->sent_at;
            done++;
            if (bench_now_ns() < deadline && send_request(cn) != 0)
                return -1;
        }
    }

    *requests = done;
    return (bench_now_ns() - t0) / 1e9;
}

static double percentile(double *v, long n, double p)
{
    long idx;

    if (n == 0)
        return 0.0;
    idx = (long)(p * (double)(n - 1));
    return v[idx];
}

struct outcome {
    double rps_median, rps_lo, rps_hi;
    double p50_us, p99_us;
    int    connected;
    int    ok;
};

static void run_config(int green, int conns, int port, struct outcome *out)
{
    struct conn *c = calloc((size_t)conns, sizeof(*c));
    double rps[BENCH_RUNS];
    pid_t  pid;
    int    epfd, i;

    memset(out, 0, sizeof(*out));
    if (c == NULL)
        exit(1);

    pid  = start_server(green, port, conns);
    epfd = epoll_create1(0);
    if (epfd < 0)
        exit(1);

    /* Give the server time to bind, then connect everything once and keep the
     * connections up for every run of this configuration. */
    usleep(300000);
    out->connected = connect_all(c, conns, port, epfd);
    if (out->connected != conns) {
        fprintf(stderr, "  only %d of %d connections established\n",
                out->connected, conns);
        goto cleanup;
    }
    usleep(300000);

    nsamples = 0;
    for (i = 0; i < BENCH_WARMUP; i++) {
        long req = 0;
        if (run_window(c, conns, epfd, 0.3, &req) < 0)
            goto cleanup;
    }
    nsamples = 0; /* the warmup's latencies are discarded with its throughput */

    for (i = 0; i < BENCH_RUNS; i++) {
        long   req = 0;
        double secs = run_window(c, conns, epfd, WINDOW_SEC, &req);

        if (secs < 0)
            goto cleanup;
        rps[i] = (double)req / secs;
    }

    bench_stats(rps, BENCH_RUNS, &out->rps_median, &out->rps_lo, &out->rps_hi);
    qsort(samples, (size_t)nsamples, sizeof(*samples), bench_cmp);
    out->p50_us = percentile(samples, nsamples, 0.50) / 1000.0;
    out->p99_us = percentile(samples, nsamples, 0.99) / 1000.0;
    out->ok = 1;

cleanup:
    for (i = 0; i < conns; i++)
        if (c[i].fd >= 0)
            close(c[i].fd);
    close(epfd);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    free(c);
}

int main(void)
{
    struct outcome res[2][NCONFIGS];
    int port = BASE_PORT;
    int k, i;
    FILE *md;

    signal(SIGPIPE, SIG_IGN);
    if (bench_pin_cpu(1) != 0)
        perror("warning: sched_setaffinity");

    samples = malloc(sizeof(double) * MAX_SAMPLES);
    if (samples == NULL)
        return 1;

    printf("\necho server throughput and latency\n");
    bench_print_machine(stdout);
    printf("server pinned to CPU 0, load generator to CPU 1\n");
    printf("%d byte request and reply, %d windows of %.1fs per configuration\n\n",
           PAYLOAD, BENCH_RUNS, WINDOW_SEC);
    fflush(stdout);

    for (k = 0; k < 2; k++) {
        for (i = 0; i < NCONFIGS; i++) {
            printf("  %-7s %5d connections ... ", k == 0 ? "green" : "pthread",
                   conn_counts[i]);
            fflush(stdout);
            run_config(k == 0, conn_counts[i], port++, &res[k][i]);
            if (res[k][i].ok)
                printf("%9.0f req/s   p50 %6.1f us   p99 %7.1f us\n",
                       res[k][i].rps_median, res[k][i].p50_us,
                       res[k][i].p99_us);
            else
                printf("did not complete (%d of %d connections)\n",
                       res[k][i].connected, conn_counts[i]);
            fflush(stdout);
        }
    }

    md = bench_results_open();
    fprintf(md, "## Echo server, end to end\n\n");
    fprintf(md, "```\n");
    bench_print_machine(md);
    fprintf(md, "server pinned to CPU 0, load generator to CPU 1\n");
    fprintf(md, "%d byte request and reply, %d windows of %.1fs per "
                "configuration\n", PAYLOAD, BENCH_RUNS, WINDOW_SEC);
    fprintf(md, "```\n\n");
    fprintf(md, "| server | connections | req/s (median) | min | max | p50 (us) "
                "| p99 (us) |\n");
    fprintf(md, "|---|---|---|---|---|---|---|\n");
    for (k = 0; k < 2; k++) {
        for (i = 0; i < NCONFIGS; i++) {
            const struct outcome *o = &res[k][i];

            if (o->ok)
                fprintf(md, "| %s | %d | %.0f | %.0f | %.0f | %.1f | %.1f |\n",
                        k == 0 ? "green" : "pthread", conn_counts[i],
                        o->rps_median, o->rps_lo, o->rps_hi, o->p50_us,
                        o->p99_us);
            else
                fprintf(md, "| %s | %d | did not complete | | | | |\n",
                        k == 0 ? "green" : "pthread", conn_counts[i]);
        }
    }
    fprintf(md, "\nBoth servers are confined to a single core so that the "
                "comparison is concurrency against concurrency. The pthread "
                "server would scale across cores on a real deployment; this "
                "runtime cannot, and that is the honest limitation of N:1.\n\n");
    fclose(md);

    free(samples);
    return 0;
}
