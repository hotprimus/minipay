#include "processor.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "journal.h"
#include "luhn.h"

struct processor {
    pthread_mutex_t lock;
    account_store *store;
    journal journal;
    mp_clock_fn clock;
    uint64_t seq;
    proc_stats stats;
};

typedef struct {
    char rrn[13];
    char auth_code[7];
    bool has_auth_code;
    bool has_balance;
    int64_t balance;
} decision;

static time_t default_clock(void)
{
    return time(NULL);
}

const char *rc_describe(const char *rc)
{
    static const struct {
        const char *code;
        const char *text;
    } table[] = {
        {RC_APPROVED, "Approved"},
        {RC_DO_NOT_HONOR, "Do not honor"},
        {RC_INVALID_TXN, "Invalid transaction"},
        {RC_INVALID_AMOUNT, "Invalid amount"},
        {RC_INVALID_CARD, "Invalid card number"},
        {RC_NO_ORIGINAL, "Unable to locate original transaction"},
        {RC_FORMAT_ERROR, "Format error"},
        {RC_INSUFFICIENT, "Insufficient funds"},
        {RC_EXPIRED, "Expired card"},
        {RC_RESTRICTED, "Restricted card"},
        {RC_DUPLICATE, "Duplicate transmission"},
        {RC_SYSTEM_ERROR, "System malfunction"},
    };
    if (rc == NULL)
        return "(none)";
    for (size_t i = 0; i < MP_ARRAY_LEN(table); i++)
        if (strcmp(table[i].code, rc) == 0)
            return table[i].text;
    return "Unknown response code";
}

processor *processor_create(account_store *store)
{
    if (store == NULL)
        return NULL;
    processor *p = calloc(1, sizeof *p);
    if (p == NULL)
        return NULL;
    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        free(p);
        return NULL;
    }
    p->store = store;
    journal_init(&p->journal);
    p->clock = default_clock;
    return p;
}

void processor_destroy(processor *p)
{
    if (p == NULL)
        return;
    pthread_mutex_destroy(&p->lock);
    journal_free(&p->journal);
    store_destroy(p->store);
    free(p);
}

void processor_set_clock(processor *p, mp_clock_fn clock)
{
    pthread_mutex_lock(&p->lock);
    p->clock = clock ? clock : default_clock;
    pthread_mutex_unlock(&p->lock);
}

static bool response_mti(const char *req, char out[ISO_MTI_LEN + 1])
{
    if (strlen(req) != ISO_MTI_LEN || (req[2] != '0' && req[2] != '2'))
        return false;
    memcpy(out, req, ISO_MTI_LEN + 1);
    out[2] = (char)(req[2] + 1);
    return true;
}

static int64_t digits_to_i64(const char *s, size_t n)
{
    int64_t v = 0;
    for (size_t i = 0; i < n; i++)
        v = v * 10 + (s[i] - '0');
    return v;
}

static bool card_expired(const char *yymm, time_t now)
{
    struct tm tm;
    if (gmtime_r(&now, &tm) == NULL)
        return true;
    int current = (tm.tm_year % 100) * 100 + (tm.tm_mon + 1);
    int expiry = (int)digits_to_i64(yymm, 4);
    return expiry < current;
}

static void copy_str(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src);
}

static bool has_all(const iso_msg *m, const int *fields, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!iso_has(m, fields[i]))
            return false;
    return true;
}

static const char *handle_auth(processor *p, const iso_msg *req, decision *d)
{
    static const int required[] = {F_PAN, F_PROC_CODE, F_AMOUNT, F_TRANSMISSION_DT,
                                   F_STAN, F_EXPIRY, F_TERMINAL_ID};
    if (!has_all(req, required, MP_ARRAY_LEN(required)))
        return RC_FORMAT_ERROR;

    const char *pan = iso_get(req, F_PAN);
    const char *proc = iso_get(req, F_PROC_CODE);
    const char *tid = iso_get(req, F_TERMINAL_ID);
    const char *stan = iso_get(req, F_STAN);
    const char *dt = iso_get(req, F_TRANSMISSION_DT);

    bool purchase = strncmp(proc, "00", 2) == 0;
    bool inquiry = strncmp(proc, "31", 2) == 0;
    if (!purchase && !inquiry)
        return RC_INVALID_TXN;

    int64_t amount = digits_to_i64(iso_get(req, F_AMOUNT), 12);
    if (purchase && amount == 0)
        return RC_INVALID_AMOUNT;

    if (!luhn_valid(pan))
        return RC_INVALID_CARD;
    account *acc = store_find(p->store, pan);
    if (acc == NULL)
        return RC_INVALID_CARD;
    if (acc->status == CARD_BLOCKED)
        return RC_RESTRICTED;
    if (strcmp(acc->expiry, iso_get(req, F_EXPIRY)) != 0)
        return RC_DO_NOT_HONOR;
    if (card_expired(acc->expiry, p->clock()))
        return RC_EXPIRED;

    if (inquiry) {
        d->has_balance = true;
        d->balance = acc->balance;
        return RC_APPROVED;
    }

    if (journal_find(&p->journal, tid, stan, dt) != NULL)
        return RC_DUPLICATE;
    if (acc->balance < amount)
        return RC_INSUFFICIENT;

    snprintf(d->auth_code, sizeof d->auth_code, "%06" PRIu64, p->seq % 1000000u);

    journal_entry e;
    memset(&e, 0, sizeof e);
    copy_str(e.tid, sizeof e.tid, tid);
    copy_str(e.stan, sizeof e.stan, stan);
    copy_str(e.datetime, sizeof e.datetime, dt);
    copy_str(e.pan, sizeof e.pan, pan);
    copy_str(e.auth_code, sizeof e.auth_code, d->auth_code);
    copy_str(e.rrn, sizeof e.rrn, d->rrn);
    e.amount = amount;

    acc->balance -= amount;
    if (journal_append(&p->journal, &e) != MP_OK) {
        acc->balance += amount;
        return RC_SYSTEM_ERROR;
    }
    d->has_auth_code = true;
    return RC_APPROVED;
}

static const char *handle_reversal(processor *p, const iso_msg *req)
{
    static const int required[] = {F_PAN, F_AMOUNT, F_STAN, F_TERMINAL_ID, F_ORIGINAL_DATA};
    if (!has_all(req, required, MP_ARRAY_LEN(required)))
        return RC_FORMAT_ERROR;

    const char *orig = iso_get(req, F_ORIGINAL_DATA);
    char ostan[7], odt[11];
    memcpy(ostan, orig + 4, 6);
    ostan[6] = '\0';
    memcpy(odt, orig + 10, 10);
    odt[10] = '\0';

    journal_entry *e = journal_find(&p->journal, iso_get(req, F_TERMINAL_ID), ostan, odt);
    if (e == NULL || strcmp(e->pan, iso_get(req, F_PAN)) != 0)
        return RC_NO_ORIGINAL;
    if (digits_to_i64(iso_get(req, F_AMOUNT), 12) != e->amount)
        return RC_INVALID_AMOUNT;

    if (e->reversed)
        return RC_APPROVED;

    account *acc = store_find(p->store, e->pan);
    if (acc == NULL)
        return RC_SYSTEM_ERROR;
    acc->balance += e->amount;
    e->reversed = true;
    p->stats.reversals++;
    return RC_APPROVED;
}

static const char *handle_network(const iso_msg *req)
{
    const char *code = iso_get(req, F_NETWORK_CODE);
    if (code == NULL)
        return RC_FORMAT_ERROR;
    if (strcmp(code, "001") == 0 || strcmp(code, "002") == 0 || strcmp(code, "301") == 0)
        return RC_APPROVED;
    return RC_INVALID_TXN;
}

mp_status processor_handle(processor *p, const iso_msg *req, iso_msg *resp)
{
    if (p == NULL || req == NULL || resp == NULL)
        return MP_ERR_ARG;
    char rmti[ISO_MTI_LEN + 1];
    if (!response_mti(req->mti, rmti))
        return MP_ERR_FORMAT;

    iso_msg_free(resp);
    mp_status st = iso_set_mti(resp, rmti);
    static const int echo[] = {F_PAN, F_PROC_CODE, F_AMOUNT, F_TRANSMISSION_DT, F_STAN,
                               F_TERMINAL_ID, F_MERCHANT_ID, F_CURRENCY, F_NETWORK_CODE,
                               F_ORIGINAL_DATA};
    for (size_t i = 0; st == MP_OK && i < MP_ARRAY_LEN(echo); i++)
        st = iso_copy_field(resp, req, echo[i]);
    if (st != MP_OK)
        goto fail;

    decision d;
    memset(&d, 0, sizeof d);
    const char *rc;
    bool network = strcmp(req->mti, "0800") == 0;

    pthread_mutex_lock(&p->lock);
    p->seq++;
    snprintf(d.rrn, sizeof d.rrn, "%012" PRIu64, p->seq % 1000000000000u);
    if (strcmp(req->mti, "0100") == 0 || strcmp(req->mti, "0200") == 0)
        rc = handle_auth(p, req, &d);
    else if (strcmp(req->mti, "0400") == 0)
        rc = handle_reversal(p, req);
    else if (network)
        rc = handle_network(req);
    else
        rc = RC_INVALID_TXN;
    p->stats.total++;
    if (strcmp(rc, RC_APPROVED) == 0)
        p->stats.approved++;
    else
        p->stats.declined++;
    pthread_mutex_unlock(&p->lock);

    if (!network)
        st = iso_set(resp, F_RRN, d.rrn);
    if (st == MP_OK && d.has_auth_code)
        st = iso_set(resp, F_AUTH_CODE, d.auth_code);
    if (st == MP_OK && d.has_balance) {
        const char *cur = iso_has(req, F_CURRENCY) ? iso_get(req, F_CURRENCY) : "643";
        st = iso_setf(resp, F_ADDL_AMOUNTS, "0002%sC%012" PRId64, cur, d.balance);
    }
    if (st == MP_OK)
        st = iso_set(resp, F_RESP_CODE, rc);
    if (st != MP_OK)
        goto fail;
    return MP_OK;

fail:
    iso_msg_free(resp);
    return st;
}

void processor_get_stats(processor *p, proc_stats *out)
{
    pthread_mutex_lock(&p->lock);
    *out = p->stats;
    pthread_mutex_unlock(&p->lock);
}

mp_status processor_balance(processor *p, const char *pan, int64_t *out)
{
    if (p == NULL || pan == NULL || out == NULL)
        return MP_ERR_ARG;
    pthread_mutex_lock(&p->lock);
    account *a = store_find(p->store, pan);
    if (a != NULL)
        *out = a->balance;
    pthread_mutex_unlock(&p->lock);
    return a != NULL ? MP_OK : MP_ERR_NOTFOUND;
}

void processor_foreach_account(processor *p, void (*fn)(const account *a, void *ctx), void *ctx)
{
    if (p == NULL || fn == NULL)
        return;
    pthread_mutex_lock(&p->lock);
    store_foreach(p->store, fn, ctx);
    pthread_mutex_unlock(&p->lock);
}
