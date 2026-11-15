#ifndef MP_PROCESSOR_H
#define MP_PROCESSOR_H

#include <stdint.h>
#include <time.h>

#include "accounts.h"
#include "common.h"
#include "iso8583.h"

#define RC_APPROVED "00"
#define RC_DO_NOT_HONOR "05"
#define RC_INVALID_TXN "12"
#define RC_INVALID_AMOUNT "13"
#define RC_INVALID_CARD "14"
#define RC_NO_ORIGINAL "25"
#define RC_FORMAT_ERROR "30"
#define RC_INSUFFICIENT "51"
#define RC_EXPIRED "54"
#define RC_RESTRICTED "62"
#define RC_DUPLICATE "94"
#define RC_SYSTEM_ERROR "96"

const char *rc_describe(const char *rc);

typedef time_t (*mp_clock_fn)(void);

typedef struct {
    uint64_t total;
    uint64_t approved;
    uint64_t declined;
    uint64_t reversals;
} proc_stats;

typedef struct processor processor;

processor *processor_create(account_store *store);
void processor_destroy(processor *p);
void processor_set_clock(processor *p, mp_clock_fn clock);

mp_status processor_handle(processor *p, const iso_msg *req, iso_msg *resp);

void processor_get_stats(processor *p, proc_stats *out);
mp_status processor_balance(processor *p, const char *pan, int64_t *out);

void processor_foreach_account(processor *p, void (*fn)(const account *a, void *ctx), void *ctx);

#endif
