#include "accounts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct account_store {
    account **buckets;
    size_t nbuckets;
    size_t count;
};

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 14695981039346656037ULL;
    for (; *s != '\0'; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

static size_t bucket_of(const account_store *s, const char *pan)
{
    return (size_t)(fnv1a(pan) & (uint64_t)(s->nbuckets - 1));
}

account_store *store_create(size_t initial_buckets)
{
    size_t n = 16;
    while (n < initial_buckets) {
        if (n > SIZE_MAX / 2)
            return NULL;
        n <<= 1;
    }
    account_store *s = malloc(sizeof *s);
    if (s == NULL)
        return NULL;
    s->buckets = calloc(n, sizeof *s->buckets);
    if (s->buckets == NULL) {
        free(s);
        return NULL;
    }
    s->nbuckets = n;
    s->count = 0;
    return s;
}

void store_destroy(account_store *s)
{
    if (s == NULL)
        return;
    for (size_t i = 0; i < s->nbuckets; i++) {
        account *a = s->buckets[i];
        while (a != NULL) {
            account *next = a->next;
            free(a);
            a = next;
        }
    }
    free(s->buckets);
    free(s);
}

static mp_status grow(account_store *s)
{
    if (s->nbuckets > SIZE_MAX / 2 / sizeof(account *))
        return MP_ERR_NOMEM;
    size_t n = s->nbuckets * 2;
    account **nb = calloc(n, sizeof *nb);
    if (nb == NULL)
        return MP_ERR_NOMEM;
    for (size_t i = 0; i < s->nbuckets; i++) {
        account *a = s->buckets[i];
        while (a != NULL) {
            account *next = a->next;
            size_t idx = (size_t)(fnv1a(a->pan) & (uint64_t)(n - 1));
            a->next = nb[idx];
            nb[idx] = a;
            a = next;
        }
    }
    free(s->buckets);
    s->buckets = nb;
    s->nbuckets = n;
    return MP_OK;
}

static int is_digits(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

mp_status store_add(account_store *s, const char *pan, const char *expiry, int64_t balance,
                    card_status status)
{
    if (s == NULL || pan == NULL || expiry == NULL || balance < 0)
        return MP_ERR_ARG;
    if (!luhn_valid(pan) || strlen(expiry) != 4 || !is_digits(expiry, 4))
        return MP_ERR_ARG;
    if (store_find(s, pan) != NULL)
        return MP_ERR_EXISTS;

    if (s->count + 1 > s->nbuckets / 4 * 3)
        (void)grow(s);

    account *a = calloc(1, sizeof *a);
    if (a == NULL)
        return MP_ERR_NOMEM;
    memcpy(a->pan, pan, strlen(pan) + 1);
    memcpy(a->expiry, expiry, 5);
    a->balance = balance;
    a->status = status;

    size_t idx = bucket_of(s, pan);
    a->next = s->buckets[idx];
    s->buckets[idx] = a;
    s->count++;
    return MP_OK;
}

account *store_find(account_store *s, const char *pan)
{
    if (s == NULL || pan == NULL)
        return NULL;
    for (account *a = s->buckets[bucket_of(s, pan)]; a != NULL; a = a->next)
        if (strcmp(a->pan, pan) == 0)
            return a;
    return NULL;
}

size_t store_count(const account_store *s)
{
    return s ? s->count : 0;
}

void store_foreach(const account_store *s, void (*fn)(const account *a, void *ctx), void *ctx)
{
    if (s == NULL || fn == NULL)
        return;
    for (size_t i = 0; i < s->nbuckets; i++)
        for (const account *a = s->buckets[i]; a != NULL; a = a->next)
            fn(a, ctx);
}

static size_t split_fields(char *line, char **out, size_t max)
{
    size_t n = 0;
    char *p = line;
    for (;;) {
        if (n == max)
            return max + 1;
        out[n++] = p;
        char *comma = strchr(p, ',');
        if (comma == NULL)
            return n;
        *comma = '\0';
        p = comma + 1;
    }
}

static int parse_balance(const char *s, int64_t *out)
{
    if (*s == '\0')
        return 0;
    errno = 0;
    char *end;
    long long v = strtoll(s, &end, 10);
    if (errno == ERANGE || *end != '\0' || v < 0)
        return 0;
    *out = (int64_t)v;
    return 1;
}

mp_status store_load_csv(account_store *s, const char *path, size_t *loaded, size_t *err_line)
{
    if (s == NULL || path == NULL)
        return MP_ERR_ARG;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return MP_ERR_IO;

    char line[256];
    size_t lineno = 0, n = 0;
    mp_status st = MP_OK;

    while (fgets(line, sizeof line, f) != NULL) {
        lineno++;
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n' && !feof(f)) {
            st = MP_ERR_FORMAT;
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#' || strncmp(line, "pan,", 4) == 0)
            continue;

        char *fld[4];
        int64_t balance;
        card_status cs;
        if (split_fields(line, fld, 4) != 4 || !parse_balance(fld[2], &balance)) {
            st = MP_ERR_FORMAT;
            break;
        }
        if (strcmp(fld[3], "active") == 0) {
            cs = CARD_ACTIVE;
        } else if (strcmp(fld[3], "blocked") == 0) {
            cs = CARD_BLOCKED;
        } else {
            st = MP_ERR_FORMAT;
            break;
        }
        st = store_add(s, fld[0], fld[1], balance, cs);
        if (st != MP_OK)
            break;
        n++;
    }
    if (st == MP_OK && ferror(f))
        st = MP_ERR_IO;
    fclose(f);

    if (loaded != NULL)
        *loaded = n;
    if (st != MP_OK && err_line != NULL)
        *err_line = lineno;
    return st;
}
