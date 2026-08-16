/*
 * chan.c - CSP channels: buffered, unbuffered, close, and select.
 *
 * The one thing to understand before reading this file:
 *
 *   An unbuffered channel is not a queue of size zero. It is a
 *   synchronisation point that happens to carry a value.
 *
 * A buffered channel has storage, and a send is finished the moment the value
 * is in that storage; the sender and the receiver never have to be in the
 * same place at the same time. An unbuffered channel has no storage at all,
 * so the value has nowhere to live in between. What happens instead is that
 * the sender parks itself on the channel with a pointer to the value sitting
 * in its own stack frame, and the receiver that eventually turns up copies
 * straight out of that frame and then unblocks the sender. The value is never
 * anywhere except the sender's frame and then the receiver's; the channel
 * only ever holds the pointer.
 *
 * That is why the sender's send() does not return until a receiver arrives.
 * It is not a queue that happens to be full - the return from send is the
 * signal that the handoff took place, and the two threads have synchronised
 * with each other whether the program wanted that or not. It is also why the
 * sender's frame must stay alive for the whole operation, which it does,
 * because a blocked thread is not going anywhere.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "sync.h"

/*
 * A blocked channel operation. These are always allocated on the stack of the
 * thread that blocks - see gth_chan_send() - and that is not a trick, it is
 * the natural place for them: the node is needed for exactly as long as the
 * thread is parked, and a parked thread's frame is not going anywhere. It
 * also means the hot path allocates nothing at all, which matters because a
 * channel operation is the most common thing a CSP program does.
 *
 * A separate node type is needed rather than the intrusive gth.next link,
 * because gth_select() puts one thread on several channels at once.
 */
struct gth_waiter {
    struct gth_waiter *next;
    struct gth        *th;
    void              *val;    /* send: the value; recv: where to put it */
    int                op;
    int                done;   /* set by the counterpart: the value moved */
    int                closed; /* set by gth_chan_close() */
};

struct gth_chan {
    size_t         cap;       /* elements, 0 for a rendezvous channel */
    size_t         elem_size;
    size_t         count;     /* elements currently buffered */
    size_t         head;      /* next slot to read */
    size_t         tail;      /* next slot to write */
    unsigned char *buf;       /* NULL when cap == 0 */
    int            closed;
    struct gth_waiter *senders;
    struct gth_waiter *receivers;
};

gth_chan_t *gth_chan_create(size_t capacity, size_t elem_size)
{
    gth_chan_t *c;

    if (elem_size == 0) {
        errno = EINVAL;
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->cap       = capacity;
    c->elem_size = elem_size;
    if (capacity > 0) {
        c->buf = calloc(capacity, elem_size);
        if (c->buf == NULL) {
            free(c);
            return NULL;
        }
    }
    return c;
}

void gth_chan_destroy(gth_chan_t *c)
{
    if (c == NULL)
        return;
    free(c->buf);
    free(c);
}

size_t gth_chan_len(const gth_chan_t *c)
{
    return c->count;
}

/* ------------------------------------------------------------------ */
/* Wait list handling and the claim rule                              */
/* ------------------------------------------------------------------ */

static void waiter_push(struct gth_waiter **head, struct gth_waiter *w)
{
    w->next = NULL;
    while (*head != NULL)
        head = &(*head)->next;
    *head = w;
}

static void waiter_remove(struct gth_waiter **head, struct gth_waiter *w)
{
    while (*head != NULL) {
        if (*head == w) {
            *head = w->next;
            w->next = NULL;
            return;
        }
        head = &(*head)->next;
    }
    /* Not on the list: already taken off by whoever claimed the thread. */
}

/*
 * Take the first waiter whose thread has not already been claimed by another
 * channel, and mark that thread claimed.
 *
 * This is the exactly-once rule that makes select correct, and it is worth
 * being precise about what the danger actually is, because "two channels
 * become ready at the same time" sounds like a multiprocessor race and it is
 * not one - there is only one CPU here and only one thread running at a time.
 *
 * The real sequence is this. Thread S is blocked in gth_select() on channels
 * X and Y, so it has a waiter node on each. Thread P sends on X, finds S's
 * node, hands over the value and makes S runnable. Thread Q then sends on Y -
 * before S has had a chance to run, because P and Q are ordinary threads and
 * the scheduler decides when S gets the CPU - and finds S's other node, still
 * sitting on Y's list. If Q completes that one too, S is handed two values
 * and can only return one; the other is lost, and the send that produced it
 * has already returned successfully. A lost value in a CSP runtime is the
 * worst kind of bug, because it looks like an application logic error.
 *
 * The fix is a single flag on the thread, checked and set here. The first
 * claimer wins, every later one skips the node and drops it. What makes the
 * check-and-set indivisible is not an atomic instruction, it is that every
 * caller of this function holds a critical section, so the timer cannot land
 * between the read of th->claimed and the write. That is the entire mutual
 * exclusion story in this runtime: one CPU, one interrupt, mask the
 * interrupt.
 *
 * The flag is cleared by whoever is about to block, immediately before it
 * publishes its nodes, so its lifetime is exactly the interval during which
 * the thread is claimable.
 */
static struct gth_waiter *chan_claim(struct gth_waiter **head)
{
    while (*head != NULL) {
        struct gth_waiter *w = *head;

        *head = w->next;
        w->next = NULL;
        if (w->th->claimed)
            continue; /* another channel got there first, discard the node */
        w->th->claimed = 1;
        return w;
    }
    return NULL;
}

static void waiter_complete(struct gth_waiter *w)
{
    w->done = 1;
    gth_wake(w->th);
}

/* ------------------------------------------------------------------ */
/* Ring buffer                                                        */
/* ------------------------------------------------------------------ */

static void ring_put(gth_chan_t *c, const void *elem)
{
    memcpy(c->buf + c->tail * c->elem_size, elem, c->elem_size);
    c->tail = (c->tail + 1) % c->cap;
    c->count++;
}

static void ring_get(gth_chan_t *c, void *out)
{
    memcpy(out, c->buf + c->head * c->elem_size, c->elem_size);
    c->head = (c->head + 1) % c->cap;
    c->count--;
}

/* ------------------------------------------------------------------ */
/* Non-blocking cores, shared by send/recv/trysend/tryrecv/select      */
/* ------------------------------------------------------------------ */

enum gth_try {
    GTH_TRY_BLOCKED = 0, /* would have to wait */
    GTH_TRY_OK      = 1, /* a value moved */
    GTH_TRY_CLOSED  = 2  /* channel closed: no value, and none is coming */
};

/*
 * Callers hold a critical section. *woken, if set, is a thread that this
 * operation made runnable and that may deserve the CPU immediately; the
 * caller decides after releasing the section.
 */
static enum gth_try chan_try_send(gth_chan_t *c, const void *elem,
                                  struct gth **woken)
{
    struct gth_waiter *w;

    if (c->closed)
        return GTH_TRY_CLOSED;

    /*
     * A parked receiver is the fast path for both kinds of channel. On an
     * unbuffered channel it is the only path. On a buffered one a receiver
     * can only be parked when the buffer is empty, so handing the value
     * straight over is both correct and one memcpy cheaper than putting it in
     * the ring for the receiver to take out again.
     */
    w = chan_claim(&c->receivers);
    if (w != NULL) {
        memcpy(w->val, elem, c->elem_size);
        waiter_complete(w);
        *woken = w->th;
        return GTH_TRY_OK;
    }

    if (c->cap > 0 && c->count < c->cap) {
        ring_put(c, elem);
        return GTH_TRY_OK;
    }
    return GTH_TRY_BLOCKED;
}

static enum gth_try chan_try_recv(gth_chan_t *c, void *out, struct gth **woken)
{
    struct gth_waiter *w;

    if (c->count > 0) {
        ring_get(c, out);
        /*
         * The buffer was full, so a sender may have been waiting for room.
         * Taking one element out makes exactly one slot, so exactly one
         * sender gets to finish, and its value goes to the back of the ring
         * where it belongs - moving it here rather than waking the sender to
         * do it keeps the channel's ordering the same as its arrival order.
         */
        w = chan_claim(&c->senders);
        if (w != NULL) {
            ring_put(c, w->val);
            waiter_complete(w);
            *woken = w->th;
        }
        return GTH_TRY_OK;
    }

    /* No storage, or empty storage: look for a sender parked with a value. */
    w = chan_claim(&c->senders);
    if (w != NULL) {
        memcpy(out, w->val, c->elem_size);
        waiter_complete(w);
        *woken = w->th;
        return GTH_TRY_OK;
    }

    if (c->closed)
        return GTH_TRY_CLOSED;
    return GTH_TRY_BLOCKED;
}

/*
 * If the thread we just unblocked outranks us, hand it the CPU now. Called
 * with no critical section held.
 */
static void yield_to(struct gth *t)
{
    if (t != NULL && t->prio > grt.cur->prio)
        gth_yield();
}

/* ------------------------------------------------------------------ */
/* Blocking operations                                                */
/* ------------------------------------------------------------------ */

int gth_chan_send(gth_chan_t *c, const void *elem)
{
    struct gth        *self  = grt.cur;
    struct gth        *woken = NULL;
    struct gth_waiter  me;
    enum gth_try       r;

    gth_enter_critical();
    r = chan_try_send(c, elem, &woken);
    if (r != GTH_TRY_BLOCKED) {
        gth_exit_critical();
        if (r == GTH_TRY_CLOSED) {
            errno = EPIPE;
            return -1;
        }
        yield_to(woken);
        return 0;
    }

    /*
     * Park. me lives in this frame and stays valid for exactly as long as
     * this thread is blocked. The const is cast away because the receiver
     * only ever reads through this pointer; the alternative is two pointer
     * fields in the waiter, which buys nothing.
     */
    me.th     = self;
    me.val    = (void *)(uintptr_t)elem;
    me.op     = GTH_CASE_SEND;
    me.done   = 0;
    me.closed = 0;
    self->claimed = 0;
    waiter_push(&c->senders, &me);
    gth_block();
    gth_exit_critical();

    if (me.closed) {
        errno = EPIPE;
        return -1;
    }
    return 0;
}

int gth_chan_recv(gth_chan_t *c, void *elem)
{
    struct gth       *self  = grt.cur;
    struct gth       *woken = NULL;
    struct gth_waiter me;
    enum gth_try      r;

    gth_enter_critical();
    r = chan_try_recv(c, elem, &woken);
    if (r != GTH_TRY_BLOCKED) {
        gth_exit_critical();
        if (r == GTH_TRY_CLOSED)
            return 0;
        yield_to(woken);
        return 1;
    }

    me.th     = self;
    me.val    = elem;
    me.op     = GTH_CASE_RECV;
    me.done   = 0;
    me.closed = 0;
    self->claimed = 0;
    waiter_push(&c->receivers, &me);
    gth_block();
    gth_exit_critical();

    return me.done ? 1 : 0; /* not done means it was woken by close */
}

int gth_chan_trysend(gth_chan_t *c, const void *elem)
{
    struct gth  *woken = NULL;
    enum gth_try r;

    gth_enter_critical();
    r = chan_try_send(c, elem, &woken);
    gth_exit_critical();

    if (r == GTH_TRY_OK) {
        yield_to(woken);
        return 0;
    }
    errno = (r == GTH_TRY_CLOSED) ? EPIPE : EAGAIN;
    return -1;
}

int gth_chan_tryrecv(gth_chan_t *c, void *elem)
{
    struct gth  *woken = NULL;
    enum gth_try r;

    gth_enter_critical();
    r = chan_try_recv(c, elem, &woken);
    gth_exit_critical();

    if (r == GTH_TRY_OK) {
        yield_to(woken);
        return 1;
    }
    if (r == GTH_TRY_CLOSED)
        return 0;
    errno = EAGAIN;
    return -1;
}

/*
 * Closing wakes everyone. A receiver that is never woken on close is a thread
 * that hangs forever on a channel nobody will ever write to again, which is
 * the single most common way to leak a goroutine in Go and the reason this
 * loop drains both lists rather than just the receivers.
 *
 * Buffered values are deliberately left in place: a closed channel still
 * hands out what was already in it, and only reports closed once it is
 * empty. Discarding them would make close() lossy and would turn a clean
 * producer shutdown into dropped work.
 */
void gth_chan_close(gth_chan_t *c)
{
    struct gth_waiter *w;

    gth_enter_critical();
    c->closed = 1;
    while ((w = chan_claim(&c->receivers)) != NULL) {
        w->closed = 1;
        gth_wake(w->th);
    }
    while ((w = chan_claim(&c->senders)) != NULL) {
        w->closed = 1;
        gth_wake(w->th);
    }
    gth_exit_critical();
}

/* ------------------------------------------------------------------ */
/* select                                                             */
/* ------------------------------------------------------------------ */

int gth_select(gth_case_t *cases, int n)
{
    struct gth        *self  = grt.cur;
    struct gth        *woken = NULL;
    struct gth_waiter  nodes[GTH_SELECT_MAX];
    static unsigned    rotor; /* see the fairness note below */
    int i, k, fired = -1;

    if (n <= 0 || n > GTH_SELECT_MAX || cases == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (cases[i].ch == NULL ||
            (cases[i].op != GTH_CASE_SEND && cases[i].op != GTH_CASE_RECV)) {
            errno = EINVAL;
            return -1;
        }
    }

    gth_enter_critical();

    /*
     * First pass: is anything ready right now? Start at a rotating offset
     * rather than always at case 0. With a fixed order, a select in a loop
     * over a channel that is always ready would never look at the later
     * cases at all, and a slow producer on case 1 would be starved by a fast
     * one on case 0. Go randomises for the same reason; a rotor is
     * deterministic, which makes tests reproducible, and is good enough
     * because the unfairness being avoided is systematic, not adversarial.
     */
    for (k = 0; k < n; k++) {
        int idx = (int)((rotor + (unsigned)k) % (unsigned)n);
        gth_case_t *cs = &cases[idx];
        enum gth_try r = (cs->op == GTH_CASE_SEND)
                             ? chan_try_send(cs->ch, cs->val, &woken)
                             : chan_try_recv(cs->ch, cs->val, &woken);

        if (r != GTH_TRY_BLOCKED) {
            cs->ok = (r == GTH_TRY_OK);
            fired  = idx;
            break;
        }
    }
    rotor++;

    if (fired >= 0) {
        gth_exit_critical();
        yield_to(woken);
        return fired;
    }

    /*
     * Nothing ready, so register on all of them and block. claimed is cleared
     * before the nodes are published, never after: a node that is visible on
     * a channel list while claimed still holds a stale value is a node that
     * every claimer will skip, and the thread would sleep through its own
     * wakeup.
     */
    self->claimed = 0;
    for (i = 0; i < n; i++) {
        nodes[i].th     = self;
        nodes[i].val    = cases[i].val;
        nodes[i].op     = cases[i].op;
        nodes[i].done   = 0;
        nodes[i].closed = 0;
        if (cases[i].op == GTH_CASE_SEND)
            waiter_push(&cases[i].ch->senders, &nodes[i]);
        else
            waiter_push(&cases[i].ch->receivers, &nodes[i]);
    }

    gth_block();

    /*
     * Awake, and by the claim rule exactly one node has been completed. The
     * rest are still sitting on channel lists pointing at this stack frame,
     * which is about to go away, so they have to come off before returning -
     * this is the cleanup half of the exactly-once protocol and skipping it
     * would leave dangling nodes on live channels.
     */
    for (i = 0; i < n; i++) {
        if (nodes[i].done || nodes[i].closed) {
            fired = i;
            cases[i].ok = nodes[i].done;
        }
        if (cases[i].op == GTH_CASE_SEND)
            waiter_remove(&cases[i].ch->senders, &nodes[i]);
        else
            waiter_remove(&cases[i].ch->receivers, &nodes[i]);
    }
    gth_exit_critical();

    return fired;
}
