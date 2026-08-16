/*
 * stack.c - green thread stacks, each with a guard page underneath.
 *
 * A green thread's stack is just memory. Nothing in the kernel knows it is a
 * stack, so nothing in the kernel will grow it or fault cleanly when it runs
 * out: overrunning it writes straight into whatever mapping happens to sit
 * below, which in a pool like this one is the next thread's stack. That is
 * silent corruption discovered minutes later in an unrelated thread.
 *
 * The guard page is the fix. One PROT_NONE page below every stack turns the
 * first byte of overflow into a SIGSEGV at a known address, which preempt.c
 * catches and reports as "stack overflow in thread N". Cost: one page of
 * address space per thread and no resident memory at all.
 */
#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

#include "internal.h"

static size_t page_round_up(size_t n, size_t page)
{
    return (n + page - 1) & ~(page - 1);
}

/*
 * The whole pool is one mapping, carved into nslots strides of
 * [guard page][usable stack]. Two reasons for one mapping instead of nslots
 * of them: it is one syscall regardless of pool size, and it keeps the kernel's
 * VMA count down (a separate mmap per thread would give 10k VMAs for 10k
 * threads, and the kernel walks that list).
 *
 * MAP_NORESERVE plus lazy faulting is what makes a large pool affordable: the
 * mapping is address space, not memory. A thread that touches 3 pages of its
 * 64 KiB stack costs 12 KiB of RSS, not 64 KiB. bench/scale.c measures exactly
 * this difference against pthread's 8 MiB default reservation.
 */
int gth_stack_pool_init(struct gth_stack_pool *p, size_t nslots, size_t usable)
{
    long   pgsz = sysconf(_SC_PAGESIZE);
    size_t page, span, total;
    char  *base;
    size_t i;

    if (pgsz <= 0) {
        errno = EINVAL;
        return -1;
    }
    page = (size_t)pgsz;

    p->guard  = page;
    p->usable = page_round_up(usable, page);
    span      = p->guard + p->usable;
    total     = span * nslots;
    if (nslots == 0 || p->usable == 0 || total / span != nslots) {
        errno = EINVAL;
        return -1;
    }

    /*
     * MAP_STACK is advisory on Linux/x86-64 but says what this memory is for,
     * and on architectures that care it selects the right placement. The
     * region starts PROT_NONE so that a bug that computes a slot address
     * wrongly faults instead of scribbling; only the usable parts are opened.
     */
    base = mmap(NULL, total, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED)
        return -1;

    for (i = 0; i < nslots; i++) {
        char *stack = base + i * span + p->guard;
        if (mprotect(stack, p->usable, PROT_READ | PROT_WRITE) != 0) {
            int saved = errno;
            munmap(base, total);
            errno = saved;
            return -1;
        }
    }

    p->base   = base;
    p->nslots = nslots;
    p->span   = span;
    return 0;
}

void gth_stack_pool_destroy(struct gth_stack_pool *p)
{
    if (p->base != NULL) {
        munmap(p->base, p->span * p->nslots);
        p->base = NULL;
    }
}

/*
 * Lowest usable byte of slot i. The stack grows down from
 * gth_stack_slot(i) + usable towards this address, and the page below it is
 * the guard.
 */
void *gth_stack_slot(const struct gth_stack_pool *p, size_t i)
{
    return (char *)p->base + i * p->span + p->guard;
}
