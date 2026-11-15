#ifndef MP_ACCOUNTS_H
#define MP_ACCOUNTS_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "luhn.h"

typedef enum { CARD_ACTIVE = 0, CARD_BLOCKED = 1 } card_status;

typedef struct account {
    char pan[PAN_MAX_LEN + 1];
    char expiry[5];
    int64_t balance;
    card_status status;
    struct account *next;
} account;

typedef struct account_store account_store;

account_store *store_create(size_t initial_buckets);
void store_destroy(account_store *s);

mp_status store_add(account_store *s, const char *pan, const char *expiry, int64_t balance,
                    card_status status);
account *store_find(account_store *s, const char *pan);
size_t store_count(const account_store *s);

void store_foreach(const account_store *s, void (*fn)(const account *a, void *ctx), void *ctx);

mp_status store_load_csv(account_store *s, const char *path, size_t *loaded, size_t *err_line);

#endif
