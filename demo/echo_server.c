/*
 * echo_server.c - one green thread per connection, all on one OS thread.
 *
 *   ./build/echo_server [port] [max-connections]
 *
 * The code is written the way a blocking, thread-per-connection server is
 * written, because that is the easiest concurrency model to read: accept a
 * connection, hand it to a thread, and let that thread loop on read and write
 * until the peer goes away. There is no state machine, no callback, no
 * explicit event loop, and no "what was this connection in the middle of"
 * bookkeeping - the connection's state is just the position in the function,
 * held in that thread's stack.
 *
 * What makes it affordable is that none of those reads and writes actually
 * blocks the process. gth_read() returns EAGAIN under the covers, parks the
 * green thread on epoll and lets the scheduler run the other 9,999
 * connections. So this gets the readability of thread-per-connection with the
 * scaling behaviour of an event loop, which is the entire argument for green
 * threads.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gthread.h"
#include "sync.h"

#define ECHO_BUF 4096

static volatile long live_connections;
static volatile long total_connections;
static volatile long bytes_echoed;

static void *connection(void *arg)
{
    int  fd = (int)(long)arg;
    char buf[ECHO_BUF];

    live_connections++;
    total_connections++;

    for (;;) {
        ssize_t n = gth_read(fd, buf, sizeof(buf));

        if (n == 0)
            break; /* orderly shutdown by the peer */
        if (n < 0) {
            if (errno == ECONNRESET)
                break;
            perror("echo: read");
            break;
        }
        if (gth_write_all(fd, buf, (size_t)n) < 0) {
            if (errno != EPIPE && errno != ECONNRESET)
                perror("echo: write");
            break;
        }
        bytes_echoed += n;
    }

    gth_close(fd);
    live_connections--;
    return NULL;
}

static void *reporter(void *arg)
{
    (void)arg;
    for (;;) {
        gth_sleep(1000);
        printf("live %ld  total %ld  bytes %ld\n",
               live_connections, total_connections, bytes_echoed);
        fflush(stdout);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 9001;
    long maxconn = (argc > 2) ? atol(argv[2]) : 12000;
    gth_config_t cfg;
    struct sockaddr_in addr;
    int listener, one = 1;

    /*
     * Two green threads are needed per connection slot in the worst case
     * (the connection thread and headroom for accept/report), and the stacks
     * are sized down from the 64 KiB default: an echo handler's frame is a
     * 4 KiB buffer and a few locals, so 32 KiB is generous. 12,000 slots at
     * 32 KiB is 384 MiB of address space and almost none of it resident,
     * because a stack costs pages only when it is touched.
     */
    memset(&cfg, 0, sizeof(cfg));
    cfg.max_threads = (size_t)maxconn + 16;
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
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    /*
     * A deep backlog because the accept loop is one green thread among
     * thousands: during a burst it may be a few milliseconds between accepts,
     * and a short backlog would turn that into refused connections.
     */
    if (listen(listener, 4096) != 0) {
        perror("listen");
        return 1;
    }

    printf("echo server on port %d, up to %ld connections, one green thread "
           "each, one OS thread total (pid %d)\n", port, maxconn, getpid());
    fflush(stdout);

    go_spawn_detached(reporter, NULL);

    for (;;) {
        int fd = gth_accept(listener, NULL, NULL);

        if (fd < 0) {
            perror("accept");
            break;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        /*
         * Detached: nobody joins a connection handler, so its slot has to be
         * reclaimed when it returns rather than waiting for a join that is
         * never coming.
         */
        if (go_spawn_detached(connection, (void *)(long)fd) < 0) {
            /*
             * The thread pool is full, which is this server's admission
             * control. Refusing the connection is the honest response;
             * accepting it and running out of memory later is not.
             */
            fprintf(stderr, "out of thread slots, dropping connection\n");
            gth_close(fd);
        }
    }

    gth_close(listener);
    gth_shutdown();
    return 0;
}
