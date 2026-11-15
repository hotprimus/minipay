#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "accounts.h"
#include "iso8583.h"
#include "log.h"
#include "luhn.h"
#include "net.h"
#include "processor.h"
#include "threadpool.h"

static atomic_int g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    atomic_store(&g_stop, 1);
}

typedef struct {
    processor *proc;
    int idle_limit_sec;
} server_ctx;

static void log_transaction(int fd, const iso_msg *req, const iso_msg *resp)
{
    char masked[PAN_MAX_LEN + 1] = "-";
    if (iso_has(req, F_PAN))
        pan_mask(iso_get(req, F_PAN), masked, sizeof masked);
    const char *rc = iso_get(resp, F_RESP_CODE);
    const char *stan = iso_get(req, F_STAN);
    const char *amount = iso_get(req, F_AMOUNT);
    LOG_INFO("fd=%d %s->%s pan=%s stan=%s amount=%s rc=%s (%s)", fd, req->mti, resp->mti, masked,
             stan ? stan : "-", amount ? amount : "-", rc ? rc : "-", rc_describe(rc));
}

static void handle_client(int fd, void *arg)
{
    server_ctx *ctx = arg;
    uint8_t in[NET_MAX_FRAME], out[NET_MAX_FRAME];
    iso_msg req, resp;
    iso_msg_init(&req);
    iso_msg_init(&resp);

    if (net_set_timeouts(fd, 5) != MP_OK)
        LOG_SYS("setsockopt");

    int idle = 0;
    while (!atomic_load(&g_stop)) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int r = poll(&pfd, 1, 1000);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            LOG_SYS("poll(client)");
            break;
        }
        if (r == 0) {
            if (++idle >= ctx->idle_limit_sec) {
                LOG_INFO("fd=%d idle for %d s, closing", fd, idle);
                break;
            }
            continue;
        }
        idle = 0;

        size_t len = 0;
        mp_status st = net_recv_frame(fd, in, sizeof in, &len);
        if (st == MP_ERR_CLOSED) {
            LOG_DEBUG("fd=%d peer closed", fd);
            break;
        }
        if (st != MP_OK) {
            LOG_WARN("fd=%d recv: %s", fd, mp_strerror(st));
            break;
        }
        st = iso_unpack(in, len, &req);
        if (st != MP_OK) {
            LOG_WARN("fd=%d bad ISO 8583 message (%zu bytes): %s", fd, len, mp_strerror(st));
            break;
        }
        st = processor_handle(ctx->proc, &req, &resp);
        if (st != MP_OK) {
            LOG_WARN("fd=%d cannot process MTI %s: %s", fd, req.mti, mp_strerror(st));
            break;
        }
        size_t out_len = 0;
        st = iso_pack(&resp, out, sizeof out, &out_len);
        if (st != MP_OK) {
            LOG_ERROR("fd=%d pack: %s", fd, mp_strerror(st));
            break;
        }
        log_transaction(fd, &req, &resp);
        st = net_send_frame(fd, out, out_len);
        if (st != MP_OK) {
            LOG_WARN("fd=%d send: %s", fd, mp_strerror(st));
            break;
        }
    }
    iso_msg_free(&req);
    iso_msg_free(&resp);
    close(fd);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-f cards.csv] [-H 127.0.0.1] [-p 8583] [-t threads] [-i idle_sec] [-v]\n",
            prog);
}

static int parse_long(const char *s, long min, long max, long *out)
{
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return 0;
    *out = v;
    return 1;
}

static int install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;
    sa.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &sa, NULL);
}

static void print_balance(const account *a, void *ctx)
{
    (void)ctx;
    char masked[PAN_MAX_LEN + 1];
    pan_mask(a->pan, masked, sizeof masked);
    LOG_INFO("  %-19s balance=%" PRId64 ".%02" PRId64 " %s", masked, a->balance / 100,
             a->balance % 100, a->status == CARD_BLOCKED ? "BLOCKED" : "");
}

int main(int argc, char **argv)
{
    const char *cards = "data/cards.csv";
    const char *host = "127.0.0.1";
    long port = 8583, threads = 4, idle = 30;
    int opt;
    while ((opt = getopt(argc, argv, "f:H:p:t:i:vh")) != -1) {
        switch (opt) {
        case 'f': cards = optarg; break;
        case 'H': host = optarg; break;
        case 'p':
            if (!parse_long(optarg, 1, 65535, &port)) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 't':
            if (!parse_long(optarg, 1, 256, &threads)) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'i':
            if (!parse_long(optarg, 1, 3600, &idle)) {
                usage(argv[0]);
                return 2;
            }
            break;
        case 'v': log_set_level(LVL_DEBUG); break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }

    int rc = 1;
    int lfd = -1;
    account_store *store = NULL;
    processor *proc = NULL;
    threadpool *tp = NULL;

    if (install_signals() != 0) {
        LOG_SYS("sigaction");
        return 1;
    }

    store = store_create(1024);
    if (store == NULL) {
        LOG_ERROR("out of memory");
        goto out;
    }
    size_t loaded = 0, bad_line = 0;
    mp_status st = store_load_csv(store, cards, &loaded, &bad_line);
    if (st == MP_ERR_IO) {
        LOG_SYS(cards);
        goto out;
    }
    if (st != MP_OK) {
        LOG_ERROR("%s:%zu: %s", cards, bad_line, mp_strerror(st));
        goto out;
    }
    LOG_INFO("loaded %zu cards from %s", loaded, cards);

    proc = processor_create(store);
    if (proc == NULL) {
        LOG_ERROR("cannot create processor");
        goto out;
    }
    store = NULL;

    lfd = net_listen(host, (uint16_t)port, 128);
    if (lfd < 0) {
        LOG_SYS("listen");
        goto out;
    }

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    server_ctx ctx = {.proc = proc, .idle_limit_sec = (int)idle};
    tp = tp_create((size_t)threads, 64, handle_client, &ctx);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (tp == NULL) {
        LOG_ERROR("cannot start thread pool");
        goto out;
    }
    LOG_INFO("listening on %s:%ld with %ld worker threads (Ctrl+C to stop)", host, port, threads);

    while (!atomic_load(&g_stop)) {
        struct pollfd pfd = {.fd = lfd, .events = POLLIN};
        int r = poll(&pfd, 1, 1000);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            LOG_SYS("poll");
            break;
        }
        if (r == 0)
            continue;
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
                continue;
            LOG_SYS("accept");
            continue;
        }
        LOG_DEBUG("accepted fd=%d", cfd);
        if (tp_submit(tp, cfd) != MP_OK)
            close(cfd);
    }
    LOG_INFO("shutting down...");
    rc = 0;

out:
    if (lfd >= 0)
        close(lfd);
    tp_shutdown(tp);
    if (proc != NULL) {
        proc_stats s;
        processor_get_stats(proc, &s);
        LOG_INFO("stats: total=%" PRIu64 " approved=%" PRIu64 " declined=%" PRIu64
                 " reversals=%" PRIu64,
                 s.total, s.approved, s.declined, s.reversals);
        LOG_INFO("final balances:");
        processor_foreach_account(proc, print_balance, NULL);
    }
    processor_destroy(proc);
    store_destroy(store);
    return rc;
}
