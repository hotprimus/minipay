#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "accounts.h"
#include "iso8583.h"
#include "journal.h"
#include "luhn.h"
#include "net.h"
#include "processor.h"
#include "threadpool.h"

static int g_checks, g_failed;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failed++;                                                                \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                              \
    } while (0)

#define CHECK_STR(a, b)                                                                \
    do {                                                                               \
        const char *a_ = (a), *b_ = (b);                                               \
        g_checks++;                                                                    \
        if (a_ == NULL || strcmp(a_, b_) != 0) {                                       \
            g_failed++;                                                                \
            fprintf(stderr, "  FAIL %s:%d: %s == \"%s\" (got \"%s\")\n", __FILE__,     \
                    __LINE__, #a, b_, a_ ? a_ : "NULL");                               \
        }                                                                              \
    } while (0)

#define RUN(fn)                                                                        \
    do {                                                                               \
        int before = g_failed;                                                         \
        fn();                                                                          \
        printf("%-34s %s\n", #fn, g_failed == before ? "ok" : "FAILED");              \
    } while (0)

static void test_luhn(void)
{
    CHECK(luhn_valid("4111111111111111"));
    CHECK(luhn_valid("5555555555554444"));
    CHECK(luhn_valid("378282246310005"));
    CHECK(!luhn_valid("4111111111111112"));
    CHECK(!luhn_valid("4111-1111-1111-1111"));
    CHECK(!luhn_valid("41111111111"));
    CHECK(!luhn_valid(""));
    CHECK(!luhn_valid(NULL));
    CHECK(luhn_check_digit("411111111111111") == '1');
    CHECK(luhn_check_digit("555555555555444") == '4');
    CHECK(luhn_check_digit("12a") == '\0');
}

static void test_pan_mask(void)
{
    char out[20];
    pan_mask("4111111111111111", out, sizeof out);
    CHECK_STR(out, "411111******1111");
    pan_mask("123", out, sizeof out);
    CHECK_STR(out, "***");
    char tiny[5];
    pan_mask("4111111111111111", tiny, sizeof tiny);
    CHECK_STR(tiny, "4111");
}

static void make_auth(iso_msg *m, const char *pan, const char *amount, const char *stan)
{
    iso_msg_init(m);
    iso_set_mti(m, "0100");
    iso_set(m, F_PAN, pan);
    iso_set(m, F_PROC_CODE, "000000");
    iso_set(m, F_AMOUNT, amount);
    iso_set(m, F_TRANSMISSION_DT, "1115120000");
    iso_set(m, F_STAN, stan);
    iso_set(m, F_EXPIRY, "2812");
    iso_set(m, F_TERMINAL_ID, "TERM0001");
    iso_set(m, F_CURRENCY, "643");
}

static void test_iso_roundtrip(void)
{
    iso_msg m, back;
    make_auth(&m, "4111111111111111", "000000001000", "000001");
    uint8_t buf[512];
    size_t len = 0;
    CHECK(iso_pack(&m, buf, sizeof buf, &len) == MP_OK);

    CHECK(memcmp(buf, "0100", 4) == 0);
    CHECK((buf[4] & 0x80) == 0);
    CHECK((buf[4] & 0x40) != 0);
    CHECK(memcmp(buf + 12, "164111111111111111", 18) == 0);

    iso_msg_init(&back);
    CHECK(iso_unpack(buf, len, &back) == MP_OK);
    CHECK_STR(back.mti, "0100");
    for (int f = 2; f <= ISO_MAX_FIELD; f++) {
        if (m.fields[f] == NULL)
            CHECK(back.fields[f] == NULL);
        else
            CHECK_STR(back.fields[f], m.fields[f]);
    }
    iso_msg_free(&m);
    iso_msg_free(&back);
}

static void test_iso_secondary_bitmap(void)
{
    iso_msg m, back;
    iso_msg_init(&m);
    iso_msg_init(&back);
    iso_set_mti(&m, "0800");
    iso_set(&m, F_STAN, "123456");
    iso_set(&m, F_NETWORK_CODE, "301");
    uint8_t buf[128];
    size_t len = 0;
    CHECK(iso_pack(&m, buf, sizeof buf, &len) == MP_OK);
    CHECK((buf[4] & 0x80) != 0);
    CHECK(len == 4 + 16 + 6 + 3);
    CHECK((buf[4 + 8] & 0x04) != 0);
    CHECK(iso_unpack(buf, len, &back) == MP_OK);
    CHECK_STR(iso_get(&back, F_NETWORK_CODE), "301");
    iso_msg_free(&m);
    iso_msg_free(&back);
}

static void test_iso_rejects_bad_input(void)
{
    iso_msg m, back;
    make_auth(&m, "4111111111111111", "000000001000", "000001");
    uint8_t buf[512];
    size_t len = 0;
    CHECK(iso_pack(&m, buf, sizeof buf, &len) == MP_OK);
    iso_msg_init(&back);

    int all_rejected = 1;
    for (size_t cut = 0; cut < len; cut++)
        if (iso_unpack(buf, cut, &back) != MP_ERR_FORMAT)
            all_rejected = 0;
    CHECK(all_rejected);

    uint8_t bad[512];
    memcpy(bad, buf, len);
    bad[12] = '9';
    CHECK(iso_unpack(bad, len, &back) == MP_ERR_FORMAT);

    memcpy(bad, buf, len);
    bad[14] = 'X';
    CHECK(iso_unpack(bad, len, &back) == MP_ERR_FORMAT);

    memcpy(bad, buf, len);
    bad[len] = '0';
    CHECK(iso_unpack(bad, len + 1, &back) == MP_ERR_FORMAT);

    memcpy(bad, buf, len);
    bad[4] |= 0x01;
    CHECK(iso_unpack(bad, len, &back) == MP_ERR_FORMAT);
    CHECK(back.mti[0] == '\0');

    CHECK(iso_pack(&m, buf, 20, &len) == MP_ERR_OVERFLOW);

    CHECK(iso_set(&m, F_AMOUNT, "12") == MP_ERR_FORMAT);
    CHECK(iso_set(&m, F_PAN, "41111111111111111111") == MP_ERR_FORMAT);
    CHECK(iso_set(&m, 1, "x") == MP_ERR_ARG);
    CHECK(iso_set(&m, 8, "x") == MP_ERR_ARG);
    CHECK(iso_set_mti(&m, "01A0") == MP_ERR_ARG);
    CHECK_STR(iso_get(&m, F_AMOUNT), "000000001000");
    iso_msg_free(&m);
    iso_msg_free(&back);
}

static void test_store_basic(void)
{
    account_store *s = store_create(4);
    CHECK(s != NULL);
    CHECK(store_add(s, "4111111111111111", "2812", 1000, CARD_ACTIVE) == MP_OK);
    CHECK(store_add(s, "4111111111111111", "2812", 1000, CARD_ACTIVE) == MP_ERR_EXISTS);
    CHECK(store_add(s, "4111111111111112", "2812", 1000, CARD_ACTIVE) == MP_ERR_ARG);
    CHECK(store_add(s, "5555555555554444", "27x6", 1000, CARD_ACTIVE) == MP_ERR_ARG);
    CHECK(store_add(s, "5555555555554444", "2706", -1, CARD_ACTIVE) == MP_ERR_ARG);
    account *a = store_find(s, "4111111111111111");
    CHECK(a != NULL && a->balance == 1000);
    CHECK(store_find(s, "5555555555554444") == NULL);
    store_destroy(s);
}

static int64_t g_sum;
static void sum_balances(const account *a, void *ctx)
{
    (void)ctx;
    g_sum += a->balance;
}

static void test_store_grows(void)
{
    account_store *s = store_create(1);
    char pan[20];
    int ok = 1;
    for (int i = 0; i < 5000; i++) {
        snprintf(pan, sizeof pan, "400000%09d", i);
        pan[15] = luhn_check_digit(pan);
        pan[16] = '\0';
        if (store_add(s, pan, "3001", i, CARD_ACTIVE) != MP_OK)
            ok = 0;
    }
    CHECK(ok);
    CHECK(store_count(s) == 5000);
    snprintf(pan, sizeof pan, "400000%09d", 4321);
    pan[15] = luhn_check_digit(pan);
    pan[16] = '\0';
    account *a = store_find(s, pan);
    CHECK(a != NULL && a->balance == 4321);
    g_sum = 0;
    store_foreach(s, sum_balances, NULL);
    CHECK(g_sum == 4999LL * 5000 / 2);
    store_destroy(s);
}

static void test_store_csv(void)
{
    char path[] = "/tmp/minipay-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    const char *csv = "# comment\npan,expiry,balance,status\n"
                      "4111111111111111,2812,1500000,active\r\n"
                      "378282246310005,2701,5,blocked\n"
                      "\n";
    CHECK(write(fd, csv, strlen(csv)) == (ssize_t)strlen(csv));
    close(fd);

    account_store *s = store_create(16);
    size_t loaded = 0, bad = 0;
    CHECK(store_load_csv(s, path, &loaded, &bad) == MP_OK);
    CHECK(loaded == 2);
    account *a = store_find(s, "378282246310005");
    CHECK(a != NULL && a->status == CARD_BLOCKED && a->balance == 5);
    store_destroy(s);

    FILE *f = fopen(path, "w");
    fputs("4111111111111111,2812,12abc,active\n", f);
    fclose(f);
    s = store_create(16);
    CHECK(store_load_csv(s, path, &loaded, &bad) == MP_ERR_FORMAT);
    CHECK(bad == 1);
    store_destroy(s);
    unlink(path);

    s = store_create(16);
    CHECK(store_load_csv(s, "/nonexistent/cards.csv", &loaded, &bad) == MP_ERR_IO);
    store_destroy(s);
}

static void test_journal(void)
{
    journal j;
    journal_init(&j);
    journal_entry e;
    memset(&e, 0, sizeof e);
    strcpy(e.tid, "TERM0001");
    strcpy(e.datetime, "1115120000");
    for (int i = 0; i < 1000; i++) {
        snprintf(e.stan, sizeof e.stan, "%06u", (unsigned)i % 1000000u);
        e.amount = i;
        CHECK(journal_append(&j, &e) == MP_OK);
    }
    CHECK(j.len == 1000 && j.cap >= 1000);
    journal_entry *found = journal_find(&j, "TERM0001", "000777", "1115120000");
    CHECK(found != NULL && found->amount == 777);
    CHECK(journal_find(&j, "TERM0002", "000777", "1115120000") == NULL);
    journal_free(&j);
    CHECK(j.items == NULL && j.len == 0);
}

static time_t fake_clock(void)
{
    return (time_t)1794744000;
}

static processor *make_processor(void)
{
    account_store *s = store_create(16);
    store_add(s, "4111111111111111", "2812", 100000, CARD_ACTIVE);
    store_add(s, "5555555555554444", "2706", 500, CARD_ACTIVE);
    store_add(s, "378282246310005", "2701", 100000, CARD_BLOCKED);
    store_add(s, "4012888888881881", "2312", 100000, CARD_ACTIVE);
    processor *p = processor_create(s);
    processor_set_clock(p, fake_clock);
    return p;
}

static const char *ask(processor *p, const iso_msg *req, iso_msg *resp)
{
    if (processor_handle(p, req, resp) != MP_OK)
        return NULL;
    return iso_get(resp, F_RESP_CODE);
}

static void test_processor_auth(void)
{
    processor *p = make_processor();
    iso_msg req, resp;
    iso_msg_init(&resp);

    make_auth(&req, "4111111111111111", "000000010000", "000001");
    CHECK_STR(ask(p, &req, &resp), RC_APPROVED);
    CHECK_STR(resp.mti, "0110");
    CHECK(iso_has(&resp, F_AUTH_CODE));
    CHECK(iso_has(&resp, F_RRN));
    CHECK_STR(iso_get(&resp, F_STAN), "000001");
    int64_t bal = 0;
    CHECK(processor_balance(p, "4111111111111111", &bal) == MP_OK && bal == 90000);

    CHECK_STR(ask(p, &req, &resp), RC_DUPLICATE);
    CHECK(processor_balance(p, "4111111111111111", &bal) == MP_OK && bal == 90000);
    iso_msg_free(&req);

    make_auth(&req, "4111111111111111", "000000090001", "000002");
    CHECK_STR(ask(p, &req, &resp), RC_INSUFFICIENT);
    iso_msg_free(&req);

    make_auth(&req, "4111111111111112", "000000000100", "000003");
    CHECK_STR(ask(p, &req, &resp), RC_INVALID_CARD);
    iso_msg_free(&req);

    make_auth(&req, "378282246310005", "000000000100", "000004");
    iso_set(&req, F_EXPIRY, "2701");
    CHECK_STR(ask(p, &req, &resp), RC_RESTRICTED);
    iso_msg_free(&req);

    make_auth(&req, "4012888888881881", "000000000100", "000005");
    iso_set(&req, F_EXPIRY, "2312");
    CHECK_STR(ask(p, &req, &resp), RC_EXPIRED);
    iso_msg_free(&req);

    make_auth(&req, "5555555555554444", "000000000100", "000006");
    CHECK_STR(ask(p, &req, &resp), RC_DO_NOT_HONOR);
    iso_msg_free(&req);

    make_auth(&req, "4111111111111111", "000000000000", "000007");
    CHECK_STR(ask(p, &req, &resp), RC_INVALID_AMOUNT);
    iso_msg_free(&req);

    make_auth(&req, "4111111111111111", "000000000100", "000008");
    iso_unset(&req, F_STAN);
    CHECK_STR(ask(p, &req, &resp), RC_FORMAT_ERROR);
    iso_msg_free(&req);

    make_auth(&req, "4111111111111111", "000000000100", "000009");
    iso_set_mti(&req, "0110");
    CHECK(processor_handle(p, &req, &resp) == MP_ERR_FORMAT);
    iso_msg_free(&req);

    proc_stats st;
    processor_get_stats(p, &st);
    CHECK(st.total == 9 && st.approved == 1 && st.declined == 8);

    iso_msg_free(&resp);
    processor_destroy(p);
}

static void test_processor_balance_inquiry(void)
{
    processor *p = make_processor();
    iso_msg req, resp;
    iso_msg_init(&resp);
    make_auth(&req, "4111111111111111", "000000000000", "000010");
    iso_set(&req, F_PROC_CODE, "310000");
    CHECK_STR(ask(p, &req, &resp), RC_APPROVED);
    CHECK_STR(iso_get(&resp, F_ADDL_AMOUNTS), "0002643C000000100000");
    iso_msg_free(&req);
    iso_msg_free(&resp);
    processor_destroy(p);
}

static void make_reversal(iso_msg *m, const char *amount, const char *orig_stan)
{
    iso_msg_init(m);
    iso_set_mti(m, "0400");
    iso_set(m, F_PAN, "4111111111111111");
    iso_set(m, F_AMOUNT, amount);
    iso_set(m, F_TRANSMISSION_DT, "1115120100");
    iso_set(m, F_STAN, "000099");
    iso_set(m, F_TERMINAL_ID, "TERM0001");
    iso_setf(m, F_ORIGINAL_DATA, "0100%s1115120000%022d", orig_stan, 0);
}

static void test_processor_reversal(void)
{
    processor *p = make_processor();
    iso_msg req, resp;
    iso_msg_init(&resp);
    make_auth(&req, "4111111111111111", "000000025000", "000042");
    CHECK_STR(ask(p, &req, &resp), RC_APPROVED);
    iso_msg_free(&req);

    int64_t bal = 0;
    processor_balance(p, "4111111111111111", &bal);
    CHECK(bal == 75000);

    make_reversal(&req, "000000000001", "000042");
    CHECK_STR(ask(p, &req, &resp), RC_INVALID_AMOUNT);
    iso_msg_free(&req);

    make_reversal(&req, "000000025000", "000042");
    CHECK_STR(ask(p, &req, &resp), RC_APPROVED);
    CHECK_STR(resp.mti, "0410");
    processor_balance(p, "4111111111111111", &bal);
    CHECK(bal == 100000);

    CHECK_STR(ask(p, &req, &resp), RC_APPROVED);
    processor_balance(p, "4111111111111111", &bal);
    CHECK(bal == 100000);
    iso_msg_free(&req);

    make_reversal(&req, "000000025000", "000043");
    CHECK_STR(ask(p, &req, &resp), RC_NO_ORIGINAL);
    iso_msg_free(&req);

    proc_stats st;
    processor_get_stats(p, &st);
    CHECK(st.reversals == 1);
    iso_msg_free(&resp);
    processor_destroy(p);
}

static void test_processor_echo(void)
{
    processor *p = make_processor();
    iso_msg req, resp;
    iso_msg_init(&req);
    iso_msg_init(&resp);
    iso_set_mti(&req, "0800");
    iso_set(&req, F_STAN, "000001");
    iso_set(&req, F_NETWORK_CODE, "301");
    CHECK_STR(ask(p, &req, &resp), RC_APPROVED);
    CHECK_STR(resp.mti, "0810");
    CHECK_STR(iso_get(&resp, F_NETWORK_CODE), "301");
    CHECK(!iso_has(&resp, F_RRN));
    iso_set(&req, F_NETWORK_CODE, "999");
    CHECK_STR(ask(p, &req, &resp), RC_INVALID_TXN);
    iso_msg_free(&req);
    iso_msg_free(&resp);
    processor_destroy(p);
}

#define N_THREADS 8
#define N_PER_THREAD 1000

typedef struct {
    processor *p;
    int id;
    int approved;
} hammer_arg;

static void *hammer(void *arg)
{
    hammer_arg *h = arg;
    char tid[9], stan[7];
    snprintf(tid, sizeof tid, "TERM%04u", (unsigned)h->id % 10000u);
    for (int i = 0; i < N_PER_THREAD; i++) {
        iso_msg req, resp;
        make_auth(&req, "4111111111111111", "000000000010", "000000");
        snprintf(stan, sizeof stan, "%06u", (unsigned)i % 1000000u);
        iso_set(&req, F_STAN, stan);
        iso_set(&req, F_TERMINAL_ID, tid);
        iso_msg_init(&resp);
        const char *rc = ask(h->p, &req, &resp);
        if (rc != NULL && strcmp(rc, RC_APPROVED) == 0)
            h->approved++;
        iso_msg_free(&req);
        iso_msg_free(&resp);
    }
    return NULL;
}

static void test_processor_concurrency(void)
{
    processor *p = make_processor();
    pthread_t th[N_THREADS];
    hammer_arg args[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        args[i] = (hammer_arg){.p = p, .id = i};
        pthread_create(&th[i], NULL, hammer, &args[i]);
    }
    int approved = 0;
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(th[i], NULL);
        approved += args[i].approved;
    }
    CHECK(approved == N_THREADS * N_PER_THREAD);
    int64_t bal = 0;
    processor_balance(p, "4111111111111111", &bal);
    CHECK(bal == 100000 - 10 * N_THREADS * N_PER_THREAD);
    processor_destroy(p);
}

static void test_net_frames(void)
{
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const uint8_t msg[] = "0800hello";
    CHECK(net_send_frame(sv[0], msg, 9) == MP_OK);
    uint8_t buf[64];
    size_t len = 0;
    CHECK(net_recv_frame(sv[1], buf, sizeof buf, &len) == MP_OK);
    CHECK(len == 9 && memcmp(buf, msg, 9) == 0);

    const uint8_t huge[] = {0x03, 0xE8};
    CHECK(write(sv[0], huge, 2) == 2);
    CHECK(net_recv_frame(sv[1], buf, sizeof buf, &len) == MP_ERR_OVERFLOW);

    const uint8_t partial[] = {0x00, 0x05, 'a', 'b'};
    CHECK(write(sv[0], partial, 4) == 4);
    close(sv[0]);
    CHECK(net_recv_frame(sv[1], buf, sizeof buf, &len) == MP_ERR_CLOSED);
    close(sv[1]);
}

static atomic_int g_tasks_done;
static atomic_long g_tasks_sum;

static void count_task(int item, void *ctx)
{
    (void)ctx;
    atomic_fetch_add(&g_tasks_done, 1);
    atomic_fetch_add(&g_tasks_sum, item);
}

static void test_threadpool(void)
{
    atomic_store(&g_tasks_done, 0);
    atomic_store(&g_tasks_sum, 0);
    threadpool *tp = tp_create(4, 8, count_task, NULL);
    CHECK(tp != NULL);
    int ok = 1;
    for (int i = 1; i <= 10000; i++)
        if (tp_submit(tp, i) != MP_OK)
            ok = 0;
    CHECK(ok);
    tp_shutdown(tp);
    CHECK(atomic_load(&g_tasks_done) == 10000);
    CHECK(atomic_load(&g_tasks_sum) == 10000L * 10001 / 2);
    CHECK(tp_create(0, 8, count_task, NULL) == NULL);
}

int main(void)
{
    RUN(test_luhn);
    RUN(test_pan_mask);
    RUN(test_iso_roundtrip);
    RUN(test_iso_secondary_bitmap);
    RUN(test_iso_rejects_bad_input);
    RUN(test_store_basic);
    RUN(test_store_grows);
    RUN(test_store_csv);
    RUN(test_journal);
    RUN(test_processor_auth);
    RUN(test_processor_balance_inquiry);
    RUN(test_processor_reversal);
    RUN(test_processor_echo);
    RUN(test_processor_concurrency);
    RUN(test_net_frames);
    RUN(test_threadpool);
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
