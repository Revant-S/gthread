/*
 * M5 acceptance: 5,000 simultaneous TCP connections, correctly echoed, on one
 * OS thread.
 *
 * Server and clients are all green threads in this one process, which makes
 * the test self-contained and makes the claim sharper rather than weaker: at
 * the high water mark there are 5,000 connected sockets, 5,000 server side
 * green threads and 5,000 client side ones, 10,001 threads of control in a
 * single-threaded process, and every one of them is written as straight line
 * blocking code.
 *
 * Each client sends a unique payload and checks that exactly its own payload
 * comes back, so a netpoller that woke the wrong thread would be caught here
 * rather than showing up as a mysterious latency.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gthread.h"
#include "sync.h"

#define NCLIENTS   5000
#define PAYLOAD    64
#define BASE_PORT  19001

static int  listener;
static int  server_port;
static long clients_ok, clients_failed, peak_live, live;
static gth_mutex_t     stats_lock;
static gth_waitgroup_t clients_done;
static gth_waitgroup_t all_connected;

/*
 * Every counter in this test goes through the mutex, and that is not
 * ceremony. A green thread can be preempted between the load and the store of
 * count++, and with 10,000 threads doing it that is not a rare event: an
 * earlier version of this test used bare increments and lost enough of them
 * to make the barrier below never fire. One OS thread is not the same thing
 * as one thread of control.
 */
static void bump(long *counter, long delta)
{
    gth_mutex_lock(&stats_lock);
    *counter += delta;
    if (counter == &live && live > peak_live)
        peak_live = live;
    gth_mutex_unlock(&stats_lock);
}

static void *server_conn(void *arg)
{
    int  fd = (int)(long)arg;
    char buf[PAYLOAD];
    ssize_t n, got = 0;

    bump(&live, 1);

    /* Read exactly PAYLOAD bytes ... */
    while (got < PAYLOAD) {
        n = gth_read(fd, buf + got, (size_t)(PAYLOAD - got));
        if (n <= 0) {
            gth_wg_done(&all_connected);
            goto out;
        }
        got += n;
    }

    /*
     * ... then hold the connection open until every other client has
     * connected and been read too. Without this barrier the server would echo
     * and close each connection as it arrived, and the test would measure a
     * rolling window of a few hundred sockets rather than NCLIENTS of them
     * open at once, which is the property being claimed.
     */
    gth_wg_done(&all_connected);
    gth_wg_wait(&all_connected);

    gth_write_all(fd, buf, PAYLOAD);
out:
    bump(&live, -1);
    gth_close(fd);
    return NULL;
}

static void *acceptor(void *arg)
{
    long accepted = 0;

    (void)arg;
    /*
     * Exactly NCLIENTS and then return. The runtime has no way to cancel a
     * green thread parked on an fd, so an acceptor looping forever would
     * still be parked on the listener at the end of the test and nothing
     * would ever join it.
     */
    while (accepted < NCLIENTS) {
        int fd = gth_accept(listener, NULL, NULL);

        if (fd < 0) {
            perror("accept");
            break;
        }
        if (go_spawn_detached(server_conn, (void *)(long)fd) < 0) {
            perror("go_spawn_detached");
            gth_close(fd);
            break;
        }
        accepted++;
    }
    return NULL;
}

static void *client(void *arg)
{
    long id = (long)arg;
    struct sockaddr_in addr;
    char out[PAYLOAD], in[PAYLOAD];
    int  fd;
    ssize_t n, got = 0;

    memset(out, 0, sizeof(out));
    snprintf(out, sizeof(out), "client-%ld", id);

    fd = gth_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        goto fail;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)server_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (gth_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        goto fail_close;

    /*
     * Every client writes, then waits for all the others to connect before
     * reading, so that all 5,000 connections really are open at the same
     * time rather than being served in a rolling window.
     */
    if (gth_write_all(fd, out, PAYLOAD) != PAYLOAD)
        goto fail_close;

    while (got < PAYLOAD) {
        n = gth_read(fd, in + got, (size_t)(PAYLOAD - got));
        if (n <= 0)
            goto fail_close;
        got += n;
    }
    if (memcmp(in, out, PAYLOAD) != 0) {
        fprintf(stderr, "client %ld: echoed data did not match\n", id);
        goto fail_close;
    }

    gth_close(fd);
    bump(&clients_ok, 1);
    gth_wg_done(&clients_done);
    return NULL;

fail_close:
    gth_close(fd);
fail:
    if (clients_failed < 5)
        fprintf(stderr, "client %ld failed: %s\n", id, strerror(errno));
    bump(&clients_failed, 1);
    gth_wg_done(&clients_done);
    return NULL;
}

int main(void)
{
    gth_config_t cfg;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int one = 1, acc, i;

    /*
     * Two green threads and two sockets per connection, plus slack. Stacks
     * are 32 KiB rather than the 64 KiB default because these threads hold a
     * 64 byte buffer and little else.
     */
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_threads = 2 * NCLIENTS + 64;
    cfg.stack_size  = 32 * 1024;
    if (gth_init_config(&cfg) != 0) {
        perror("gth_init_config");
        return 1;
    }

    listener = gth_socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        return 1;
    }
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* let the kernel pick a free port */
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(listener, 4096) != 0) {
        perror("listen");
        return 1;
    }
    if (getsockname(listener, (struct sockaddr *)&addr, &alen) != 0) {
        perror("getsockname");
        return 1;
    }
    server_port = ntohs(addr.sin_port);
    printf("echo server listening on 127.0.0.1:%d\n", server_port);

    gth_mutex_init(&stats_lock);
    gth_wg_init(&clients_done);
    gth_wg_add(&clients_done, NCLIENTS);
    gth_wg_init(&all_connected);
    gth_wg_add(&all_connected, NCLIENTS);
    acc = go_spawn(acceptor, NULL);

    for (i = 0; i < NCLIENTS; i++) {
        if (go_spawn_detached(client, (void *)(long)i) < 0) {
            perror("go_spawn_detached");
            return 1;
        }
    }

    gth_wg_wait(&clients_done);

    gth_join(acc, NULL);
    gth_close(listener);

    printf("clients: %ld ok, %ld failed\n", clients_ok, clients_failed);
    printf("peak simultaneous server side connections: %ld\n", peak_live);

    if (clients_failed != 0 || clients_ok != NCLIENTS) {
        fprintf(stderr, "FAIL: %ld of %d clients did not complete\n",
                clients_failed, NCLIENTS);
        return 1;
    }
    if (peak_live < NCLIENTS) {
        fprintf(stderr, "FAIL: peak concurrency was %ld, expected %d "
                "simultaneous connections\n", peak_live, NCLIENTS);
        return 1;
    }

    printf("PASS: %d simultaneous connections echoed correctly on one OS "
           "thread\n", NCLIENTS);
    gth_shutdown();
    return 0;
}
