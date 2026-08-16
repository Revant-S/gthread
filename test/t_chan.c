/*
 * M4 acceptance.
 *
 * 1. A buffered channel moves every value exactly once, in order.
 * 2. An unbuffered send does not return until a receiver has taken the value.
 *    This is checked with an ordered log, not with timing: the receiver
 *    deliberately arrives late, and the only acceptable interleaving is
 *    "sender blocks, receiver arrives, receiver takes it, sender returns".
 * 3. close() wakes blocked receivers, drains what is buffered first, and
 *    turns further sends into errors.
 * 4. select picks up whichever channel becomes ready, and when several are
 *    ready at once it completes exactly one of them - no value is lost.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "sync.h"

/* ---------------- buffered ---------------- */

#define NVALUES 10000

static gth_chan_t *buffered;

static void *buf_producer(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < NVALUES; i++) {
        if (gth_chan_send(buffered, &i) != 0) {
            perror("send");
            return (void *)1;
        }
    }
    gth_chan_close(buffered);
    return NULL;
}

static int check_buffered(void)
{
    int  tid, v, expect = 0, got;
    void *rv = NULL;

    buffered = gth_chan_create(8, sizeof(int));
    if (buffered == NULL)
        return 1;
    tid = go_spawn(buf_producer, NULL);

    while ((got = gth_chan_recv(buffered, &v)) == 1) {
        if (v != expect) {
            fprintf(stderr, "FAIL: buffered channel gave %d, expected %d\n",
                    v, expect);
            return 1;
        }
        expect++;
    }
    gth_join(tid, &rv);
    if (rv != NULL || expect != NVALUES) {
        fprintf(stderr, "FAIL: received %d of %d values\n", expect, NVALUES);
        return 1;
    }
    gth_chan_destroy(buffered);
    printf("buffered channel: %d values, in order, none lost\n", NVALUES);
    return 0;
}

/* ---------------- unbuffered rendezvous ---------------- */

static gth_chan_t *rendezvous;
static const char *events[8];
static int         nevents;

static void note(const char *what)
{
    if (nevents < 8)
        events[nevents++] = what;
}

static void *rv_sender(void *arg)
{
    int v = 42;

    (void)arg;
    note("sender: about to send");
    gth_chan_send(rendezvous, &v);
    note("sender: send returned");
    return NULL;
}

static void *rv_receiver(void *arg)
{
    int v = 0;

    (void)arg;
    /* Arrive late on purpose, so the sender is definitely parked first. */
    gth_sleep(50);
    note("receiver: about to receive");
    gth_chan_recv(rendezvous, &v);
    note("receiver: got the value");
    return (void *)(long)v;
}

static int check_rendezvous(void)
{
    static const char *want[4] = {
        "sender: about to send",
        "receiver: about to receive",
        "receiver: got the value",
        "sender: send returned"
    };
    int   st, rt, i;
    void *rv = NULL;

    rendezvous = gth_chan_create(0, sizeof(int));
    if (rendezvous == NULL)
        return 1;

    st = go_spawn(rv_sender, NULL);
    rt = go_spawn(rv_receiver, NULL);
    gth_join(st, NULL);
    gth_join(rt, &rv);

    printf("rendezvous event order:\n");
    for (i = 0; i < nevents; i++)
        printf("  %d. %s\n", i + 1, events[i]);

    if (nevents != 4) {
        fprintf(stderr, "FAIL: %d events, expected 4\n", nevents);
        return 1;
    }
    for (i = 0; i < 4; i++) {
        if (strcmp(events[i], want[i]) != 0) {
            fprintf(stderr, "FAIL: event %d was \"%s\", expected \"%s\"\n",
                    i + 1, events[i], want[i]);
            return 1;
        }
    }
    if ((long)rv != 42) {
        fprintf(stderr, "FAIL: receiver got %ld\n", (long)rv);
        return 1;
    }
    gth_chan_destroy(rendezvous);
    printf("unbuffered send stayed blocked until the receiver arrived\n");
    return 0;
}

/* ---------------- close ---------------- */

static gth_chan_t *closing;
static int         closed_recv_result = -1;

static void *close_waiter(void *arg)
{
    int v = 0;

    (void)arg;
    /* First recv drains the buffered 7 and returns at once ... */
    if (gth_chan_recv(closing, &v) != 1 || v != 7)
        return (void *)1;
    /* ... the second finds the channel empty and parks until close(). */
    closed_recv_result = gth_chan_recv(closing, &v);
    return NULL;
}

static int check_close(void)
{
    int  tid, v, r;

    closing = gth_chan_create(4, sizeof(int));
    v = 7;
    gth_chan_send(closing, &v);

    tid = go_spawn(close_waiter, NULL);
    gth_sleep(20); /* let it drain the 7 and then block on the empty channel */
    gth_chan_close(closing);
    gth_join(tid, NULL);

    if (closed_recv_result != 0) {
        fprintf(stderr, "FAIL: blocked receiver returned %d after close, "
                "expected 0\n", closed_recv_result);
        return 1;
    }
    if (gth_chan_send(closing, &v) == 0 || errno != EPIPE) {
        fprintf(stderr, "FAIL: send on a closed channel was accepted\n");
        return 1;
    }
    r = gth_chan_recv(closing, &v);
    if (r != 0) {
        fprintf(stderr, "FAIL: recv on a drained closed channel returned %d\n",
                r);
        return 1;
    }
    gth_chan_destroy(closing);
    printf("close: blocked receiver woken with \"closed\", further sends "
           "EPIPE\n");
    return 0;
}

/* ---------------- select ---------------- */

#define SELECT_ROUNDS 2000

static gth_chan_t *sel_a, *sel_b;

static void *sel_feeder(void *arg)
{
    gth_chan_t *ch = arg;
    int i;

    for (i = 0; i < SELECT_ROUNDS; i++) {
        if (gth_chan_send(ch, &i) != 0)
            return (void *)1;
    }
    return NULL;
}

static int check_select(void)
{
    int  ta, tb, i;
    long from_a = 0, from_b = 0;
    int  va = 0, vb = 0;

    /*
     * Both feeders push as fast as they can into unbuffered channels, so the
     * selector routinely wakes to find both sides ready. If the claim rule in
     * chan_claim() were wrong, a value would be handed to this thread and
     * dropped, and the totals below would not add up.
     */
    sel_a = gth_chan_create(0, sizeof(int));
    sel_b = gth_chan_create(0, sizeof(int));
    ta = go_spawn(sel_feeder, sel_a);
    tb = go_spawn(sel_feeder, sel_b);

    for (i = 0; i < 2 * SELECT_ROUNDS; i++) {
        gth_case_t cs[2];
        int which;

        cs[0].ch = sel_a; cs[0].op = GTH_CASE_RECV; cs[0].val = &va; cs[0].ok = 0;
        cs[1].ch = sel_b; cs[1].op = GTH_CASE_RECV; cs[1].val = &vb; cs[1].ok = 0;

        which = gth_select(cs, 2);
        if (which < 0) {
            perror("gth_select");
            return 1;
        }
        if (!cs[which].ok) {
            fprintf(stderr, "FAIL: select reported a closed channel\n");
            return 1;
        }
        if (which == 0) {
            if (va != from_a) {
                fprintf(stderr, "FAIL: channel a gave %d, expected %ld\n",
                        va, from_a);
                return 1;
            }
            from_a++;
        } else {
            if (vb != from_b) {
                fprintf(stderr, "FAIL: channel b gave %d, expected %ld\n",
                        vb, from_b);
                return 1;
            }
            from_b++;
        }
    }

    gth_join(ta, NULL);
    gth_join(tb, NULL);
    gth_chan_destroy(sel_a);
    gth_chan_destroy(sel_b);

    if (from_a != SELECT_ROUNDS || from_b != SELECT_ROUNDS) {
        fprintf(stderr, "FAIL: select took %ld from a and %ld from b\n",
                from_a, from_b);
        return 1;
    }
    printf("select: %d values over two unbuffered channels, %ld from each, "
           "none lost or repeated\n", 2 * SELECT_ROUNDS, from_a);
    return 0;
}

int main(void)
{
    if (gth_init() != 0) {
        perror("gth_init");
        return 1;
    }
    if (check_buffered() != 0)
        return 1;
    if (check_rendezvous() != 0)
        return 1;
    if (check_close() != 0)
        return 1;
    if (check_select() != 0)
        return 1;

    printf("PASS: channels, rendezvous, close and select behave\n");
    gth_shutdown();
    return 0;
}
