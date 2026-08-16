/*
 * netpoll.c - epoll integration, so that blocking I/O does not block the
 * process.
 *
 * This is the problem that makes or breaks an N:1 runtime. The kernel sees
 * one thread of execution. When a green thread calls read(2) on a socket with
 * no data, the kernel puts *the OS thread* to sleep, and the OS thread is
 * every green thread in the process. One connection with nothing to say stops
 * the scheduler, the timer handler still fires but has nothing it can switch
 * to, and ten thousand other connections wait. A threading library that gets
 * everything else right and gets this wrong is a library that cannot be used
 * for the one workload it is obviously for.
 *
 * The fix has two halves. Every fd is put in O_NONBLOCK mode, so the kernel
 * never sleeps on our behalf: it returns EAGAIN instead. And when it does,
 * the calling green thread registers the fd with epoll and parks, so the
 * scheduler is free to run something else. The scheduler polls epoll on its
 * way round the loop and unparks whatever became ready. From inside the green
 * thread, gth_read() looked exactly like read(); from outside, the process
 * never blocked.
 *
 * This is the same trick Go plays. A goroutine doing a socket read calls into
 * the runtime, which parks it on a netpoll descriptor and schedules another
 * goroutine on the same OS thread; the difference is that Go also has M:N
 * scheduling underneath, so a goroutine that does block the OS thread (a
 * genuine blocking syscall, a page fault) only costs one of many.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "internal.h"

/*
 * One entry per fd, indexed by the fd number itself. Linux allocates the
 * lowest free descriptor, so this array stays as dense as the process's fd
 * usage - and an O(1) lookup on the wakeup path is worth more than the memory
 * a hash table would save.
 */
struct gth_pollfd {
    struct gth *reader;
    struct gth *writer;
    uint32_t    events;     /* what is currently registered with epoll */
    int         registered;
};

static struct gth_pollfd *pollfds;
static size_t             pollfds_cap;

static int gth_netpoll_grow(int fd)
{
    size_t need = (size_t)fd + 1;
    size_t cap  = pollfds_cap ? pollfds_cap : 64;
    struct gth_pollfd *p;

    if (need <= pollfds_cap)
        return 0;
    while (cap < need)
        cap *= 2;
    p = realloc(pollfds, cap * sizeof(*p));
    if (p == NULL)
        return -1;
    memset(p + pollfds_cap, 0, (cap - pollfds_cap) * sizeof(*p));
    pollfds     = p;
    pollfds_cap = cap;
    return 0;
}

int gth_netpoll_init(void)
{
    /*
     * EPOLL_CLOEXEC so that an exec'd child does not inherit the runtime's
     * poller.
     */
    grt.epfd = epoll_create1(EPOLL_CLOEXEC);
    if (grt.epfd < 0)
        return -1;

    /*
     * Writing to a socket whose peer has gone away raises SIGPIPE, whose
     * default action is to kill the process. For a server that is absurd -
     * clients disconnect mid-response all the time - and it is not something
     * the caller can defend against, because the check-then-write window is
     * always open. Ignoring it turns the same event into a write() that
     * returns EPIPE, which the caller can handle like any other error.
     */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

void gth_netpoll_shutdown(void)
{
    if (grt.epfd >= 0) {
        close(grt.epfd);
        grt.epfd = -1;
    }
    free(pollfds);
    pollfds     = NULL;
    pollfds_cap = 0;
}

/* Recompute the epoll registration for fd from who is actually parked on it. */
static int gth_netpoll_sync(int fd)
{
    struct gth_pollfd *p = &pollfds[fd];
    uint32_t want = 0;
    struct epoll_event ev;

    if (p->reader != NULL)
        want |= EPOLLIN;
    if (p->writer != NULL)
        want |= EPOLLOUT;

    if (want == p->events)
        return 0;

    memset(&ev, 0, sizeof(ev));
    /*
     * Level triggered, deliberately. With level triggering, "readable" is a
     * state: if the fd still has data when the next epoll_wait runs, it is
     * reported again, so a handler that reads less than everything available
     * simply gets told again. Edge triggering reports only the transition,
     * which means every reader is obliged to drain the fd until EAGAIN before
     * going back to the poller - miss that once and the connection hangs with
     * data sitting in the socket buffer and no event coming to tell you.
     *
     * Edge triggering is the faster of the two when a single thread multiplexes
     * many fds, because it does not re-report fds that are already known to be
     * ready. That advantage is much smaller here: interest is deregistered
     * the moment the parked thread is woken, so an fd is never in the epoll
     * set with nobody waiting on it, and there is nothing to re-report.
     * Level triggering costs us almost nothing and removes a whole class of
     * bug from every caller of gth_read().
     */
    ev.events  = want;
    ev.data.fd = fd;

    if (want == 0) {
        if (epoll_ctl(grt.epfd, EPOLL_CTL_DEL, fd, &ev) != 0 && errno != ENOENT)
            return -1;
        p->registered = 0;
    } else if (!p->registered) {
        if (epoll_ctl(grt.epfd, EPOLL_CTL_ADD, fd, &ev) != 0)
            return -1;
        p->registered = 1;
    } else {
        if (epoll_ctl(grt.epfd, EPOLL_CTL_MOD, fd, &ev) != 0)
            return -1;
    }
    p->events = want;
    return 0;
}

/*
 * Park the caller until fd is readable (or writable). Returns 0 when the fd
 * is ready, -1 if the registration itself failed.
 */
static int gth_park_on(int fd, int for_write)
{
    struct gth *self = grt.cur;
    struct gth_pollfd *p;
    int rc = 0;

    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    gth_enter_critical();
    if (gth_netpoll_grow(fd) != 0) {
        gth_exit_critical();
        return -1;
    }
    p = &pollfds[fd];

    /*
     * One reader and one writer per fd. Two green threads reading the same
     * socket is a data race at the application level anyway - they would
     * split the byte stream between them - so this refuses it rather than
     * silently interleaving.
     */
    if ((for_write && p->writer != NULL) || (!for_write && p->reader != NULL)) {
        gth_exit_critical();
        errno = EBUSY;
        return -1;
    }
    if (for_write)
        p->writer = self;
    else
        p->reader = self;

    if (gth_netpoll_sync(fd) != 0) {
        if (for_write)
            p->writer = NULL;
        else
            p->reader = NULL;
        gth_exit_critical();
        return -1;
    }
    grt.netpoll_parked++;
    gth_block();
    gth_exit_critical();
    return rc;
}

/*
 * Called by the scheduler. timeout_ms is 0 for a poll and a real value (or -1
 * for "until something happens") when there is nothing else to run.
 */
void gth_netpoll(int timeout_ms)
{
    struct epoll_event evs[64];
    sigset_t unmasked;
    int n, i;

    if (grt.epfd < 0 || grt.netpoll_parked == 0)
        return;

    /*
     * epoll_pwait rather than epoll_wait, and not for convenience. The
     * scheduler runs with SIGALRM blocked, so a plain epoll_wait with a long
     * timeout would sit there with the timer masked: the tick counter would
     * stop advancing (repeated expirations of a POSIX timer collapse into one
     * pending signal), and every sleep deadline in the system would be
     * measured against a clock that stopped. epoll_pwait installs the
     * unmasked set for the duration of the wait and restores it atomically on
     * return, which is the same lost-wakeup argument as sigsuspend.
     */
    sigprocmask(SIG_SETMASK, NULL, &unmasked);
    sigdelset(&unmasked, SIGALRM);

    n = epoll_pwait(grt.epfd, evs, (int)(sizeof(evs) / sizeof(evs[0])),
                    timeout_ms, &unmasked);
    if (n < 0) {
        /*
         * EINTR is not an error here, it is the timer tick arriving during
         * the wait, which happens constantly with a 10 ms period. Anything
         * else is a bug in the runtime, and there is nothing useful to do
         * with it in the scheduler, so it is ignored the same way - the next
         * pass will try again.
         */
        return;
    }

    for (i = 0; i < n; i++) {
        int fd = evs[i].data.fd;
        uint32_t e = evs[i].events;
        struct gth_pollfd *p;

        if (fd < 0 || (size_t)fd >= pollfds_cap)
            continue;
        p = &pollfds[fd];

        /*
         * EPOLLHUP and EPOLLERR are not requested and arrive anyway. They
         * have to wake both sides: a thread parked on a socket whose peer
         * vanished must be released so its next read() can return 0 or the
         * error, otherwise it waits forever for a readiness that is never
         * coming.
         */
        if (p->reader != NULL && (e & (EPOLLIN | EPOLLHUP | EPOLLERR))) {
            struct gth *t = p->reader;
            p->reader = NULL;
            grt.netpoll_parked--;
            gth_wake(t);
        }
        if (p->writer != NULL && (e & (EPOLLOUT | EPOLLHUP | EPOLLERR))) {
            struct gth *t = p->writer;
            p->writer = NULL;
            grt.netpoll_parked--;
            gth_wake(t);
        }
        gth_netpoll_sync(fd);
    }
}

/* ------------------------------------------------------------------ */
/* The I/O wrappers                                                   */
/* ------------------------------------------------------------------ */

int gth_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int gth_socket(int domain, int type, int protocol)
{
    return socket(domain, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
}

/*
 * The shape of every wrapper below:
 *
 *   try the syscall
 *   EINTR  -> retry immediately. With a signal every 10 ms this is not a
 *             theoretical branch; it fires all the time, and the alternative
 *             (SA_RESTART) does not cover epoll_wait anyway.
 *   EAGAIN -> the fd is not ready. Park, and try again when the poller says
 *             it is. This is the whole netpoller in one line of control flow.
 *   else   -> the caller's problem, returned unchanged.
 */
ssize_t gth_read(int fd, void *buf, size_t count)
{
    for (;;) {
        ssize_t n = read(fd, buf, count);

        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        if (gth_park_on(fd, 0) != 0)
            return -1;
    }
}

ssize_t gth_write(int fd, const void *buf, size_t count)
{
    for (;;) {
        ssize_t n = write(fd, buf, count);

        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        if (gth_park_on(fd, 1) != 0)
            return -1;
    }
}

ssize_t gth_write_all(int fd, const void *buf, size_t count)
{
    const unsigned char *p = buf;
    size_t done = 0;

    /*
     * write(2) on a socket is allowed to accept less than it was given, and
     * on a slow or backed up connection it usually does. Callers that ignore
     * that truncate their own output, which is why the echo server uses this
     * and not gth_write directly.
     */
    while (done < count) {
        ssize_t n = gth_write(fd, p + done, count - done);

        if (n < 0)
            return -1;
        if (n == 0)
            break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

int gth_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    for (;;) {
        int fd = accept4(sockfd, addr, addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (fd >= 0)
            return fd;
        if (errno == EINTR)
            continue;
        /*
         * ECONNABORTED means the client went away between the SYN and here.
         * It is not a reason to stop accepting, so it is folded into "try
         * again", which is what accept loops in every real server do.
         */
        if (errno == ECONNABORTED)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        if (gth_park_on(sockfd, 0) != 0)
            return -1;
    }
}

int gth_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    int err = 0;
    socklen_t len = sizeof(err);

    for (;;) {
        if (connect(sockfd, addr, addrlen) == 0)
            return 0;
        if (errno == EINTR)
            continue;
        /*
         * A non-blocking connect returns EINPROGRESS and completes later; the
         * socket becomes writable when it does. EALREADY is the same state
         * seen from a second call. The result is not the return value of
         * connect at all - it has to be fetched with SO_ERROR, and forgetting
         * that is how "the connect succeeded but the socket is dead" bugs
         * happen.
         */
        if (errno != EINPROGRESS && errno != EALREADY)
            return -1;
        break;
    }

    if (gth_park_on(sockfd, 1) != 0)
        return -1;
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
        return -1;
    if (err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

/*
 * Closing an fd that a green thread is parked on would leave that thread
 * waiting for an event that can never arrive, so the registration is torn
 * down first. A thread actually blocked on the fd at this moment is a bug in
 * the caller and is reported rather than papered over.
 */
int gth_close(int fd)
{
    if (fd >= 0 && (size_t)fd < pollfds_cap) {
        struct gth_pollfd *p;

        gth_enter_critical();
        p = &pollfds[fd];
        if (p->reader != NULL || p->writer != NULL) {
            gth_exit_critical();
            errno = EBUSY;
            return -1;
        }
        if (p->registered) {
            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            epoll_ctl(grt.epfd, EPOLL_CTL_DEL, fd, &ev);
            p->registered = 0;
            p->events     = 0;
        }
        gth_exit_critical();
    }
    return close(fd);
}
