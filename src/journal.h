#ifndef MP_JOURNAL_H
#define MP_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "luhn.h"

typedef struct {
    char tid[9];
    char stan[7];
    char datetime[11];
    char pan[PAN_MAX_LEN + 1];
    char auth_code[7];
    char rrn[13];
    int64_t amount;
    bool reversed;
} journal_entry;

typedef struct {
    journal_entry *items;
    size_t len;
    size_t cap;
} journal;

void journal_init(journal *j);
void journal_free(journal *j);
mp_status journal_append(journal *j, const journal_entry *e);

journal_entry *journal_find(journal *j, const char *tid, const char *stan, const char *datetime);

#endif
