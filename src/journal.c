#include "journal.h"

#include <stdlib.h>
#include <string.h>

void journal_init(journal *j)
{
    j->items = NULL;
    j->len = 0;
    j->cap = 0;
}

void journal_free(journal *j)
{
    free(j->items);
    journal_init(j);
}

mp_status journal_append(journal *j, const journal_entry *e)
{
    if (j == NULL || e == NULL)
        return MP_ERR_ARG;
    if (j->len == j->cap) {
        size_t ncap = j->cap ? j->cap * 2 : 64;
        if (ncap > SIZE_MAX / sizeof *j->items)
            return MP_ERR_NOMEM;
        journal_entry *p = realloc(j->items, ncap * sizeof *p);
        if (p == NULL)
            return MP_ERR_NOMEM;
        j->items = p;
        j->cap = ncap;
    }
    j->items[j->len++] = *e;
    return MP_OK;
}

journal_entry *journal_find(journal *j, const char *tid, const char *stan, const char *datetime)
{
    if (j == NULL || tid == NULL || stan == NULL || datetime == NULL)
        return NULL;
    for (size_t i = j->len; i-- > 0;) {
        journal_entry *e = &j->items[i];
        if (strcmp(e->stan, stan) == 0 && strcmp(e->tid, tid) == 0 &&
            strcmp(e->datetime, datetime) == 0)
            return e;
    }
    return NULL;
}
