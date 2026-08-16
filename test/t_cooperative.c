/*
 * M1 acceptance: three green threads, each printing its id in a loop.
 *
 * The point of the test is where the interleaving happens. With no preemption
 * yet, a thread runs until it calls gth_yield() and not one instruction less,
 * so the output must be a strict round robin: 1 2 3 1 2 3 ... Anything else
 * would mean control changed hands somewhere other than the yield, which at
 * this milestone would be a bug.
 */
#include <stdio.h>
#include <stdlib.h>

#include "gthread.h"

#define ROUNDS 4

static int order[3 * ROUNDS];
static int order_n;

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    int i;

    for (i = 0; i < ROUNDS; i++) {
        printf("thread %d: round %d\n", id, i);
        order[order_n++] = id;
        gth_yield();
    }
    return (void *)(long)(id * 100);
}

int main(void)
{
    int tid[3];
    int i;

    if (gth_init() != 0) {
        perror("gth_init");
        return 1;
    }

    for (i = 0; i < 3; i++) {
        tid[i] = go_spawn(worker, (void *)(long)(i + 1));
        if (tid[i] < 0) {
            perror("go_spawn");
            return 1;
        }
    }

    for (i = 0; i < 3; i++) {
        void *rv = NULL;
        if (gth_join(tid[i], &rv) != 0) {
            perror("gth_join");
            return 1;
        }
        printf("joined thread %d, returned %ld\n", tid[i], (long)rv);
        if ((long)rv != (i + 1) * 100) {
            fprintf(stderr, "FAIL: wrong return value\n");
            return 1;
        }
    }

    if (order_n != 3 * ROUNDS) {
        fprintf(stderr, "FAIL: expected %d switches, saw %d\n",
                3 * ROUNDS, order_n);
        return 1;
    }
    for (i = 0; i < order_n; i++) {
        if (order[i] != (i % 3) + 1) {
            fprintf(stderr, "FAIL: not round robin at index %d\n", i);
            return 1;
        }
    }

    printf("PASS: strict round robin, %d switches, all threads joined\n",
           order_n);
    gth_shutdown();
    return 0;
}
