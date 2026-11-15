#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "iso8583.h"
#include "net.h"
#include "processor.h"

#define TERMINAL_ID "TERM0001"
#define MERCHANT_ID "SHOP00000000001"
#define CURRENCY_RUB "643"

typedef struct {
    const char *host;
    uint16_t port;
    int verbose;
    unsigned long stan;
    const char *datetime;
} client_opts;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-H host] [-p port] [-s stan] [-d MMDDhhmmss] [-v] <command> [args]\n"
            "  echo\n"
            "  auth    <pan> <YYMM> <amount_minor>\n"
            "  balance <pan> <YYMM>\n"
            "  reverse <pan> <amount_minor> <orig_stan> <orig_datetime MMDDhhmmss>\n",
            prog);
}

static int parse_u64(const char *s, uint64_t max, uint64_t *out)
{
    if (s[0] < '0' || s[0] > '9')
        return 0;
    errno = 0;
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || *end != '\0' || v > max)
        return 0;
    *out = (uint64_t)v;
    return 1;
}

static mp_status fill_common(iso_msg *m, const client_opts *o, char dt_out[11])
{
    if (o->datetime != NULL) {
        snprintf(dt_out, 11, "%s", o->datetime);
    } else {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(dt_out, 11, "%m%d%H%M%S", &tm);
    }
    mp_status st = iso_set(m, F_TRANSMISSION_DT, dt_out);
    if (st == MP_OK)
        st = iso_setf(m, F_STAN, "%06lu", o->stan % 1000000ul);
    if (st == MP_OK)
        st = iso_set(m, F_TERMINAL_ID, TERMINAL_ID);
    if (st == MP_OK)
        st = iso_set(m, F_MERCHANT_ID, MERCHANT_ID);
    if (st == MP_OK)
        st = iso_set(m, F_CURRENCY, CURRENCY_RUB);
    return st;
}

static mp_status exchange(const client_opts *o, const iso_msg *req, iso_msg *resp)
{
    uint8_t buf[NET_MAX_FRAME];
    size_t len = 0;
    mp_status st = iso_pack(req, buf, sizeof buf, &len);
    if (st != MP_OK)
        return st;
    int fd = net_connect(o->host, o->port);
    if (fd < 0) {
        perror("connect");
        return MP_ERR_IO;
    }
    st = net_set_timeouts(fd, 10);
    if (st == MP_OK)
        st = net_send_frame(fd, buf, len);
    if (st == MP_OK)
        st = net_recv_frame(fd, buf, sizeof buf, &len);
    if (st == MP_OK)
        st = iso_unpack(buf, len, resp);
    close(fd);
    return st;
}

static void print_response(const iso_msg *resp, int verbose)
{
    if (verbose) {
        fputs("<<< response\n", stdout);
        iso_dump(resp, stdout);
    }
    const char *rc = iso_get(resp, F_RESP_CODE);
    printf("MTI %s  RC %s (%s)", resp->mti, rc ? rc : "--", rc_describe(rc));
    if (iso_has(resp, F_AUTH_CODE))
        printf("  auth=%s", iso_get(resp, F_AUTH_CODE));
    if (iso_has(resp, F_RRN))
        printf("  rrn=%s", iso_get(resp, F_RRN));
    putchar('\n');
    const char *bal = iso_get(resp, F_ADDL_AMOUNTS);
    if (bal != NULL && strlen(bal) == 20) {
        uint64_t minor = 0;
        if (parse_u64(bal + 8, UINT64_MAX, &minor))
            printf("balance: %" PRIu64 ".%02" PRIu64 " (currency %.3s)\n", minor / 100,
                   minor % 100, bal + 4);
    }
}

int main(int argc, char **argv)
{
    client_opts o = {.host = "127.0.0.1", .port = 8583};
    o.stan = (unsigned long)time(NULL) ^ (unsigned long)getpid();
    int opt;
    uint64_t tmp;
    while ((opt = getopt(argc, argv, "H:p:s:d:vh")) != -1) {
        switch (opt) {
        case 'H': o.host = optarg; break;
        case 'p':
            if (!parse_u64(optarg, 65535, &tmp) || tmp == 0) {
                usage(argv[0]);
                return 2;
            }
            o.port = (uint16_t)tmp;
            break;
        case 's':
            if (!parse_u64(optarg, 999999, &tmp)) {
                usage(argv[0]);
                return 2;
            }
            o.stan = (unsigned long)tmp;
            break;
        case 'd':
            if (strlen(optarg) != 10 || !parse_u64(optarg, 9999999999ull, &tmp)) {
                usage(argv[0]);
                return 2;
            }
            o.datetime = optarg;
            break;
        case 'v': o.verbose = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 2;
    }
    const char *cmd = argv[optind];
    char **args = argv + optind + 1;
    int nargs = argc - optind - 1;

    iso_msg req, resp;
    iso_msg_init(&req);
    iso_msg_init(&resp);
    char dt[11];
    mp_status st = fill_common(&req, &o, dt);
    if (st != MP_OK) {
        fprintf(stderr, "error: %s\n", mp_strerror(st));
        iso_msg_free(&req);
        return 1;
    }
    uint64_t amount = 0;

    if (strcmp(cmd, "echo") == 0 && nargs == 0) {
        iso_set_mti(&req, "0800");
        st = iso_set(&req, F_NETWORK_CODE, "301");
    } else if (strcmp(cmd, "auth") == 0 && nargs == 3) {
        if (!parse_u64(args[2], 999999999999ull, &amount)) {
            fprintf(stderr, "bad amount: %s\n", args[2]);
            st = MP_ERR_ARG;
        } else {
            iso_set_mti(&req, "0100");
            st = iso_set(&req, F_PAN, args[0]);
            if (st == MP_OK)
                st = iso_set(&req, F_EXPIRY, args[1]);
            if (st == MP_OK)
                st = iso_set(&req, F_PROC_CODE, "000000");
            if (st == MP_OK)
                st = iso_setf(&req, F_AMOUNT, "%012" PRIu64, amount);
        }
    } else if (strcmp(cmd, "balance") == 0 && nargs == 2) {
        iso_set_mti(&req, "0100");
        st = iso_set(&req, F_PAN, args[0]);
        if (st == MP_OK)
            st = iso_set(&req, F_EXPIRY, args[1]);
        if (st == MP_OK)
            st = iso_set(&req, F_PROC_CODE, "310000");
        if (st == MP_OK)
            st = iso_set(&req, F_AMOUNT, "000000000000");
    } else if (strcmp(cmd, "reverse") == 0 && nargs == 4) {
        uint64_t ostan = 0;
        if (!parse_u64(args[1], 999999999999ull, &amount) || !parse_u64(args[2], 999999, &ostan) ||
            strlen(args[3]) != 10) {
            fprintf(stderr, "bad reversal arguments\n");
            st = MP_ERR_ARG;
        } else {
            iso_set_mti(&req, "0400");
            st = iso_set(&req, F_PAN, args[0]);
            if (st == MP_OK)
                st = iso_setf(&req, F_AMOUNT, "%012" PRIu64, amount);
            if (st == MP_OK)
                st = iso_setf(&req, F_ORIGINAL_DATA, "0100%06" PRIu64 "%s%022d", ostan, args[3], 0);
        }
    } else {
        usage(argv[0]);
        iso_msg_free(&req);
        return 2;
    }

    if (st == MP_OK) {
        if (o.verbose) {
            fputs(">>> request\n", stdout);
            iso_dump(&req, stdout);
        }
        st = exchange(&o, &req, &resp);
    }
    if (st != MP_OK) {
        fprintf(stderr, "error: %s\n", mp_strerror(st));
    } else {
        print_response(&resp, o.verbose);
        const char *rc = iso_get(&resp, F_RESP_CODE);
        if (strcmp(cmd, "auth") == 0 && rc != NULL && strcmp(rc, RC_APPROVED) == 0)
            printf("to reverse: %s reverse %s %" PRIu64 " %s %s\n", argv[0], iso_get(&req, F_PAN),
                   amount, iso_get(&req, F_STAN), dt);
    }
    iso_msg_free(&req);
    iso_msg_free(&resp);
    return st == MP_OK ? 0 : 1;
}
