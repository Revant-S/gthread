/*
 * pipeline.c - a CSP pipeline, and a select with a timeout.
 *
 * Stage 1 generates the numbers 1..N. Stage 2 keeps the odd ones and doubles
 * them. Stage 3 adds them up and checks that every value the filter sent
 * arrived exactly once and in order.
 *
 * Three green threads, two channels, no locks anywhere. That is the point of
 * CSP: the buffer between the stages is the channel, and the only
 * synchronisation is "a value moved", so there is no shared mutable state for
 * anyone to get wrong. All of it runs on one OS thread.
 *
 * The second half shows a select over a data channel and a timer channel,
 * which is the shape of every timeout in a CSP program: there is no special
 * timeout mechanism, a timeout is just another green thread sending on
 * another channel, and select takes whichever arrives first.
 */
#include <stdio.h>
#include <stdlib.h>

#include "gthread.h"
#include "sync.h"

#define N 1000

static gth_chan_t *stage1_out; /* generator -> filter, buffered */
static gth_chan_t *stage2_out; /* filter -> consumer, unbuffered */

static void *generator(void *arg)
{
    int i;

    (void)arg;
    for (i = 1; i <= N; i++) {
        if (gth_chan_send(stage1_out, &i) != 0) {
            perror("generator: send");
            return (void *)1;
        }
    }
    /*
     * Closing is how a pipeline stage says "there is no more"; without it the
     * filter would block forever on an empty channel and the whole pipeline
     * would deadlock at the end of a successful run.
     */
    gth_chan_close(stage1_out);
    return NULL;
}

static void *filter(void *arg)
{
    int v;
    long passed = 0;

    (void)arg;
    while (gth_chan_recv(stage1_out, &v) == 1) {
        if (v % 2 == 0)
            continue;
        v *= 2;
        if (gth_chan_send(stage2_out, &v) != 0) {
            perror("filter: send");
            return (void *)1;
        }
        passed++;
    }
    gth_chan_close(stage2_out);
    printf("filter: passed %ld of %d values\n", passed, N);
    return NULL;
}

static void *consumer(void *arg)
{
    long sum = 0, count = 0;
    int  v, expect = 2; /* first odd number is 1, doubled */

    (void)arg;
    while (gth_chan_recv(stage2_out, &v) == 1) {
        if (v != expect) {
            fprintf(stderr, "consumer: got %d, expected %d\n", v, expect);
            return (void *)1;
        }
        expect += 4; /* consecutive odd numbers differ by 2, doubled by 4 */
        sum += v;
        count++;
    }
    printf("consumer: %ld values, sum %ld\n", count, sum);

    /* 2 * (1 + 3 + ... + 999) = 2 * 500^2 */
    if (count != N / 2 || sum != 2L * (N / 2) * (N / 2)) {
        fprintf(stderr, "consumer: pipeline lost or duplicated values\n");
        return (void *)1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* select with a timeout                                              */
/* ------------------------------------------------------------------ */

struct timer_arg {
    unsigned    ms;
    gth_chan_t *ch;
};

/*
 * A timer is a thread that sleeps and then sends. Its channel has room for
 * one element so that the send always completes: if it were unbuffered and
 * the select had already taken the data case, this thread would park on the
 * send forever and never be joinable.
 */
static void *timer_thread(void *arg)
{
    struct timer_arg *ta = arg;
    int token = 1;

    gth_sleep(ta->ms);
    gth_chan_trysend(ta->ch, &token);
    return NULL;
}

static gth_chan_t *slow_data;

static void *slow_producer(void *arg)
{
    int i;

    (void)arg;
    for (i = 0; i < 3; i++) {
        gth_sleep(30);
        if (gth_chan_send(slow_data, &i) != 0)
            return (void *)1;
    }
    /* Then stop sending, without closing: the consumer's timeout has to fire. */
    return NULL;
}

static int run_select_demo(void)
{
    gth_chan_t      *timeout_ch;
    struct timer_arg ta;
    int              prod, timer;
    int              rounds = 0;

    slow_data  = gth_chan_create(0, sizeof(int));
    timeout_ch = gth_chan_create(1, sizeof(int));
    if (slow_data == NULL || timeout_ch == NULL)
        return 1;

    ta.ms = 100;
    ta.ch = timeout_ch;
    prod  = go_spawn(slow_producer, NULL);
    timer = go_spawn(timer_thread, &ta);

    for (;;) {
        gth_case_t cs[2];
        int v = 0, token = 0, which;

        cs[0].ch = slow_data;  cs[0].op = GTH_CASE_RECV; cs[0].val = &v;
        cs[1].ch = timeout_ch; cs[1].op = GTH_CASE_RECV; cs[1].val = &token;
        cs[0].ok = cs[1].ok = 0;

        which = gth_select(cs, 2);
        if (which < 0) {
            perror("gth_select");
            return 1;
        }
        if (which == 0) {
            printf("select: took value %d from the data channel\n", v);
            rounds++;
        } else {
            printf("select: timed out after 100 ms with no data, "
                   "giving up\n");
            break;
        }
    }

    gth_join(prod, NULL);
    gth_join(timer, NULL);
    gth_chan_destroy(slow_data);
    gth_chan_destroy(timeout_ch);

    if (rounds != 3) {
        fprintf(stderr, "select demo: expected 3 values before the timeout, "
                "got %d\n", rounds);
        return 1;
    }
    return 0;
}

int main(void)
{
    int   g, f, c;
    void *rv1 = NULL, *rv2 = NULL, *rv3 = NULL;

    if (gth_init() != 0) {
        perror("gth_init");
        return 1;
    }

    /*
     * The first channel is buffered, the second is not, so the run exercises
     * both paths: the generator can get up to 8 values ahead of the filter,
     * but the filter and the consumer move in lockstep.
     */
    stage1_out = gth_chan_create(8, sizeof(int));
    stage2_out = gth_chan_create(0, sizeof(int));
    if (stage1_out == NULL || stage2_out == NULL) {
        perror("gth_chan_create");
        return 1;
    }

    g = go_spawn(generator, NULL);
    f = go_spawn(filter, NULL);
    c = go_spawn(consumer, NULL);
    if (g < 0 || f < 0 || c < 0) {
        perror("go_spawn");
        return 1;
    }
    gth_join(g, &rv1);
    gth_join(f, &rv2);
    gth_join(c, &rv3);
    if (rv1 != NULL || rv2 != NULL || rv3 != NULL)
        return 1;

    gth_chan_destroy(stage1_out);
    gth_chan_destroy(stage2_out);

    puts("");
    if (run_select_demo() != 0)
        return 1;

    puts("\npipeline demo finished cleanly");
    gth_shutdown();
    return 0;
}
