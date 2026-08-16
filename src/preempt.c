/*
 * preempt.c - the periodic timer, the SIGALRM handler, and critical sections.
 *
 * Preemption without the kernel scheduler is a trick with two halves. The
 * first is getting control back from a thread that has no intention of giving
 * it up: a POSIX interval timer delivers SIGALRM every 10 ms, and the kernel
 * pushes a signal frame onto whatever the thread was doing. The second is
 * making sure that frame never lands in the middle of a runtime data
 * structure, which is what critical sections are for.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "internal.h"

static timer_t   gth_timer;
static int       gth_timer_live;
static sigset_t  gth_alarm_set;   /* just SIGALRM */
static stack_t   gth_sigstack;

/* ------------------------------------------------------------------ */
/* Critical sections                                                  */
/* ------------------------------------------------------------------ */

/*
 * A critical section here is exactly an interrupt mask on a bare metal
 * system: SIGALRM is the interrupt, sigprocmask is the mask register, and the
 * run queue is the structure that must not be observed half updated. There is
 * only one CPU in this runtime, so there is nothing to spin against; the only
 * thing that can interleave with a run queue update is the signal handler,
 * and blocking the signal removes it.
 *
 * The depth counter matters because these nest. gth_chan_send() takes a
 * critical section and calls gth_wake(), which takes another; if the inner
 * exit unblocked unconditionally, the outer section would run with the timer
 * live and could be preempted with the wait list half unlinked. So the mask
 * is only touched on the 0 -> 1 and 1 -> 0 transitions.
 *
 * The counter lives in the TCB, not in a global, and that is not an
 * arbitrary choice. A thread is allowed to block while holding a critical
 * section - that is precisely what every wait primitive does - and
 * swapcontext carries the signal mask with the context. If the mask is
 * per thread then the count has to be per thread as well, or the next thread
 * to run would inherit a depth it never entered.
 */
void gth_enter_critical(void)
{
    struct gth *self = grt.cur;

    if (self->crit++ == 0)
        sigprocmask(SIG_BLOCK, &gth_alarm_set, NULL);
}

void gth_exit_critical(void)
{
    struct gth *self = grt.cur;

    if (--self->crit == 0)
        sigprocmask(SIG_UNBLOCK, &gth_alarm_set, NULL);
}

/* ------------------------------------------------------------------ */
/* The timer signal                                                   */
/* ------------------------------------------------------------------ */

/*
 * The whole handler. It counts the tick and hands the CPU to the scheduler,
 * and that is deliberately all it does.
 *
 * Async-signal-safety is the constraint that shapes this function. The
 * handler runs on top of an arbitrary instruction of an arbitrary thread, so
 * anything holding a lock or a static buffer at that moment is a trap:
 * printf can be halfway through the stdio buffer, malloc can be halfway
 * through its arena. Neither is called here, and neither is anything that
 * calls them. The temptation is to do useful work in the handler - walk the
 * sleeper list, expire timers - and the way to resist it is to note that the
 * scheduler is about to run anyway, half a microsecond from now, with the
 * timer masked and nothing half updated. So the sleeper walk lives there.
 *
 * Note what is NOT set in gth_preempt_init(): SA_ONSTACK. It is the obvious
 * flag to reach for and it would be wrong here. The handler does not return
 * normally when it preempts - it swapcontexts to the scheduler and stays
 * switched out until this thread is scheduled again, so its frame, including
 * the kernel's signal frame that a later sigreturn has to restore from, must
 * stay valid for as long as the thread is descheduled. An alternate signal
 * stack is a single shared buffer: the kernel puts the frame at the top of it
 * whenever the interrupted stack pointer is not already inside it. Preempt
 * thread A, switch to B, preempt B, and B's frame lands on the same bytes as
 * A's. Resuming A then restores B's registers into A. Leaving SA_ONSTACK off
 * puts the frame on the green thread's own stack, where nothing else can
 * touch it, and the guard page below that stack turns the one remaining
 * hazard - a signal arriving when the thread is nearly out of stack - into
 * the SIGSEGV report below rather than into silent corruption. That is what
 * the alternate stack is installed for here: it is the SIGSEGV handler that
 * needs it, because that handler is the one that has to run when the thread
 * stack is exhausted.
 */
static void gth_tick_handler(int sig)
{
    (void)sig;

    /*
     * The conversion back to sig_atomic_t is implementation defined once the
     * value passes INT_MAX (gcc defines it as wrapping, which is what the
     * deadline comparison in sched.c assumes). Doing the arithmetic in
     * uint32_t keeps it out of signed overflow, which would be undefined.
     */
    grt.ticks = (sig_atomic_t)((uint32_t)grt.ticks + 1u);

    /*
     * cur is NULL exactly while the scheduler itself is on the CPU. The
     * scheduler runs with SIGALRM blocked, so the only way to get here in
     * that state is the deliberate unblock inside sigsuspend() on the idle
     * path - where the wakeup is the point and there is nothing to preempt.
     */
    if (grt.cur == NULL)
        return;

    gth_switch_to_sched();
}

/*
 * Guard page reporting. Only reached when a green thread has run off the
 * bottom of its stack, which without this handler is a SIGSEGV with no
 * context at all. Everything in here is async-signal-safe: write(2) and
 * _exit(2), and a hand rolled hex formatter because snprintf is not.
 */
static void gth_write_hex(char *out, uintptr_t v)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    out[0] = '0';
    out[1] = 'x';
    for (i = 0; i < 16; i++)
        out[2 + i] = digits[(v >> ((15 - i) * 4)) & 0xf];
}

static void gth_segv_handler(int sig, siginfo_t *si, void *uc)
{
    static const char msg[] =
        "gthread: SIGSEGV in a green thread - fault address ";
    static const char tail[] =
        "\ngthread: this address is inside a stack guard page, so this is a "
        "green thread stack overflow, not a wild pointer\n";
    char addr[19]; /* "0x" + 16 hex digits + NUL */
    uintptr_t fault = (uintptr_t)si->si_addr;
    const struct gth_stack_pool *p = &grt.pool;
    uintptr_t base = (uintptr_t)p->base;
    int in_guard = 0;

    (void)sig;
    (void)uc;

    if (p->base != NULL && fault >= base && fault < base + p->span * p->nslots) {
        /* Offset within the stride: below p->guard means the guard page. */
        in_guard = ((fault - base) % p->span) < p->guard;
    }

    gth_write_hex(addr, fault);
    addr[18] = '\0';
    (void)!write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)!write(STDERR_FILENO, addr, 18);
    if (in_guard)
        (void)!write(STDERR_FILENO, tail, sizeof(tail) - 1);
    else
        (void)!write(STDERR_FILENO, "\n", 1);
    _exit(2);
}

int gth_preempt_init(void)
{
    struct sigaction sa;
    struct sigevent  sev;
    struct itimerspec its;

    sigemptyset(&gth_alarm_set);
    sigaddset(&gth_alarm_set, SIGALRM);

    /*
     * The alternate stack exists so that the SIGSEGV handler has somewhere to
     * run when the fault it is reporting is "this thread has no stack left".
     * A handler for that signal on the exhausted stack faults again, and the
     * second fault with no handler left is an unexplained kill.
     */
    gth_sigstack.ss_size = SIGSTKSZ * 4;
    gth_sigstack.ss_sp   = malloc(gth_sigstack.ss_size);
    gth_sigstack.ss_flags = 0;
    if (gth_sigstack.ss_sp == NULL)
        return -1;
    if (sigaltstack(&gth_sigstack, NULL) != 0)
        return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = gth_segv_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, NULL) != 0)
        return -1;
    if (sigaction(SIGBUS, &sa, NULL) != 0)
        return -1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = gth_tick_handler;
    /*
     * No SA_RESTART. With a signal every 10 ms, interrupted syscalls are the
     * normal case rather than an edge case, and the netpoller in M5 has to
     * cope with EINTR anyway because epoll_wait is never restarted. Handling
     * it in one style everywhere is easier to audit than having the kernel
     * silently restart some calls and not others.
     *
     * sa_mask is empty, so only SIGALRM itself is blocked while the handler
     * runs (the kernel adds the delivered signal automatically). That is the
     * property that keeps preemption from nesting on top of itself.
     */
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGALRM, &sa, NULL) != 0)
        return -1;

    /*
     * CLOCK_MONOTONIC, not CLOCK_REALTIME: the tick counter is used for sleep
     * deadlines and must not jump when someone sets the system clock or when
     * NTP steps it.
     */
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo  = SIGALRM;
    if (timer_create(CLOCK_MONOTONIC, &sev, &gth_timer) != 0)
        return -1;
    gth_timer_live = 1;

    /*
     * 10 ms, i.e. 100 Hz, the traditional Linux HZ. The number is a ratio
     * argument: a switch costs a few hundred nanoseconds, so a 10 ms slice
     * spends well under 0.01% of the CPU on forced switches, while still
     * bounding how long a spinning thread can hold the CPU away from a woken
     * higher priority one to something a human reads as instant. Going to
     * 1 ms would multiply the overhead by ten to improve a latency nobody in
     * this workload can perceive; going to 100 ms would make the priority
     * test in test/t_preempt.c visibly laggy.
     */
    its.it_value.tv_sec     = 0;
    its.it_value.tv_nsec    = GTH_TICK_MS * 1000L * 1000L;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = GTH_TICK_MS * 1000L * 1000L;
    if (timer_settime(gth_timer, 0, &its, NULL) != 0)
        return -1;
    return 0;
}

void gth_preempt_stop(void)
{
    if (gth_timer_live) {
        timer_delete(gth_timer);
        gth_timer_live = 0;
    }
    if (gth_sigstack.ss_sp != NULL) {
        stack_t off;
        memset(&off, 0, sizeof(off));
        off.ss_flags = SS_DISABLE;
        sigaltstack(&off, NULL);
        free(gth_sigstack.ss_sp);
        gth_sigstack.ss_sp = NULL;
    }
}
