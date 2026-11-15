#include "iso8583.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "luhn.h"

#define SPEC(nm, lt, cs, ml) {.name = (nm), .len_type = (lt), .charset = (cs), .max_len = (ml)}

static const iso_field_spec SPECS[ISO_MAX_FIELD + 1] = {
    [F_PAN] = SPEC("PAN", ISO_LLVAR, ISO_N, 19),
    [F_PROC_CODE] = SPEC("Processing code", ISO_FIXED, ISO_N, 6),
    [F_AMOUNT] = SPEC("Amount", ISO_FIXED, ISO_N, 12),
    [F_TRANSMISSION_DT] = SPEC("Transmission date/time", ISO_FIXED, ISO_N, 10),
    [F_STAN] = SPEC("STAN", ISO_FIXED, ISO_N, 6),
    [F_LOCAL_TIME] = SPEC("Local time", ISO_FIXED, ISO_N, 6),
    [F_LOCAL_DATE] = SPEC("Local date", ISO_FIXED, ISO_N, 4),
    [F_EXPIRY] = SPEC("Expiration date", ISO_FIXED, ISO_N, 4),
    [F_POS_ENTRY] = SPEC("POS entry mode", ISO_FIXED, ISO_N, 3),
    [F_RRN] = SPEC("RRN", ISO_FIXED, ISO_AN, 12),
    [F_AUTH_CODE] = SPEC("Auth ID response", ISO_FIXED, ISO_AN, 6),
    [F_RESP_CODE] = SPEC("Response code", ISO_FIXED, ISO_AN, 2),
    [F_TERMINAL_ID] = SPEC("Terminal ID", ISO_FIXED, ISO_ANS, 8),
    [F_MERCHANT_ID] = SPEC("Merchant ID", ISO_FIXED, ISO_ANS, 15),
    [F_CURRENCY] = SPEC("Currency code", ISO_FIXED, ISO_N, 3),
    [F_ADDL_AMOUNTS] = SPEC("Additional amounts", ISO_LLLVAR, ISO_ANS, 120),
    [F_NETWORK_CODE] = SPEC("Network mgmt code", ISO_FIXED, ISO_N, 3),
    [F_ORIGINAL_DATA] = SPEC("Original data elements", ISO_FIXED, ISO_N, 42),
};

const iso_field_spec *iso_spec(int field)
{
    if (field < 2 || field > ISO_MAX_FIELD)
        return NULL;
    return SPECS[field].name ? &SPECS[field] : NULL;
}

static void bitmap_set(uint8_t *bm, int field)
{
    unsigned idx = (unsigned)(field - 1);
    bm[idx / 8] = (uint8_t)(bm[idx / 8] | (0x80u >> (idx % 8)));
}

static bool bitmap_test(const uint8_t *bm, int field)
{
    unsigned idx = (unsigned)(field - 1);
    return (bm[idx / 8] & (0x80u >> (idx % 8))) != 0;
}

static bool char_ok(iso_charset cs, unsigned char c)
{
    switch (cs) {
    case ISO_N:
        return c >= '0' && c <= '9';
    case ISO_AN:
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    case ISO_ANS:
        return c >= 0x20 && c <= 0x7E;
    }
    return false;
}

static mp_status validate(const iso_field_spec *sp, const char *v, size_t len)
{
    if (sp->len_type == ISO_FIXED ? len != sp->max_len : len > sp->max_len)
        return MP_ERR_FORMAT;
    for (size_t i = 0; i < len; i++)
        if (!char_ok(sp->charset, (unsigned char)v[i]))
            return MP_ERR_FORMAT;
    return MP_OK;
}

static bool mti_valid(const char *mti)
{
    for (int i = 0; i < ISO_MTI_LEN; i++)
        if (mti[i] < '0' || mti[i] > '9')
            return false;
    return mti[ISO_MTI_LEN] == '\0';
}

static char *dup_n(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (p == NULL)
        return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void iso_msg_init(iso_msg *m)
{
    memset(m, 0, sizeof *m);
}

void iso_msg_free(iso_msg *m)
{
    if (m == NULL)
        return;
    for (int f = 0; f <= ISO_MAX_FIELD; f++) {
        free(m->fields[f]);
        m->fields[f] = NULL;
    }
    m->mti[0] = '\0';
}

mp_status iso_set_mti(iso_msg *m, const char *mti)
{
    if (m == NULL || mti == NULL || strlen(mti) != ISO_MTI_LEN)
        return MP_ERR_ARG;
    char tmp[ISO_MTI_LEN + 1];
    memcpy(tmp, mti, sizeof tmp);
    if (!mti_valid(tmp))
        return MP_ERR_ARG;
    memcpy(m->mti, tmp, sizeof tmp);
    return MP_OK;
}

mp_status iso_set(iso_msg *m, int field, const char *value)
{
    if (m == NULL || value == NULL)
        return MP_ERR_ARG;
    const iso_field_spec *sp = iso_spec(field);
    if (sp == NULL)
        return MP_ERR_ARG;
    size_t len = strlen(value);
    mp_status st = validate(sp, value, len);
    if (st != MP_OK)
        return st;
    char *copy = dup_n(value, len);
    if (copy == NULL)
        return MP_ERR_NOMEM;
    free(m->fields[field]);
    m->fields[field] = copy;
    return MP_OK;
}

mp_status iso_setf(iso_msg *m, int field, const char *fmt, ...)
{
    char tmp[ISO_MAX_VALUE_LEN + 1];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return MP_ERR_FORMAT;
    if ((size_t)n >= sizeof tmp)
        return MP_ERR_OVERFLOW;
    return iso_set(m, field, tmp);
}

void iso_unset(iso_msg *m, int field)
{
    if (m == NULL || field < 2 || field > ISO_MAX_FIELD)
        return;
    free(m->fields[field]);
    m->fields[field] = NULL;
}

const char *iso_get(const iso_msg *m, int field)
{
    if (m == NULL || field < 2 || field > ISO_MAX_FIELD)
        return NULL;
    return m->fields[field];
}

bool iso_has(const iso_msg *m, int field)
{
    return iso_get(m, field) != NULL;
}

mp_status iso_copy_field(iso_msg *dst, const iso_msg *src, int field)
{
    const char *v = iso_get(src, field);
    if (v == NULL)
        return MP_OK;
    return iso_set(dst, field, v);
}

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} writer;

static void w_put(writer *w, const void *data, size_t n)
{
    if (w->overflow)
        return;
    if (n > w->cap - w->len) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, data, n);
    w->len += n;
}

static void w_put_len(writer *w, size_t len, size_t digits)
{
    char tmp[3];
    for (size_t i = digits; i-- > 0;) {
        tmp[i] = (char)('0' + len % 10);
        len /= 10;
    }
    w_put(w, tmp, digits);
}

mp_status iso_pack(const iso_msg *m, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (m == NULL || buf == NULL || out_len == NULL)
        return MP_ERR_ARG;
    if (!mti_valid(m->mti))
        return MP_ERR_FORMAT;

    uint8_t bitmap[16] = {0};
    bool secondary = false;
    for (int f = 2; f <= ISO_MAX_FIELD; f++) {
        if (m->fields[f] == NULL)
            continue;
        bitmap_set(bitmap, f);
        if (f > 64)
            secondary = true;
    }
    if (secondary)
        bitmap_set(bitmap, 1);

    writer w = {.buf = buf, .cap = cap};
    w_put(&w, m->mti, ISO_MTI_LEN);
    w_put(&w, bitmap, secondary ? 16 : 8);

    for (int f = 2; f <= ISO_MAX_FIELD; f++) {
        const char *v = m->fields[f];
        if (v == NULL)
            continue;
        const iso_field_spec *sp = iso_spec(f);
        if (sp == NULL)
            return MP_ERR_FORMAT;
        size_t len = strlen(v);
        mp_status st = validate(sp, v, len);
        if (st != MP_OK)
            return st;
        if (sp->len_type == ISO_LLVAR)
            w_put_len(&w, len, 2);
        else if (sp->len_type == ISO_LLLVAR)
            w_put_len(&w, len, 3);
        w_put(&w, v, len);
    }
    if (w.overflow)
        return MP_ERR_OVERFLOW;
    *out_len = w.len;
    return MP_OK;
}

typedef struct {
    const uint8_t *p;
    size_t left;
} reader;

static const uint8_t *r_take(reader *r, size_t n)
{
    if (n > r->left)
        return NULL;
    const uint8_t *res = r->p;
    r->p += n;
    r->left -= n;
    return res;
}

static bool parse_digits(const uint8_t *s, size_t n, size_t *out)
{
    size_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (size_t)(s[i] - '0');
    }
    *out = v;
    return true;
}

mp_status iso_unpack(const uint8_t *buf, size_t len, iso_msg *m)
{
    if (buf == NULL || m == NULL)
        return MP_ERR_ARG;
    iso_msg_free(m);

    reader r = {.p = buf, .left = len};
    uint8_t bitmap[16] = {0};
    const uint8_t *chunk;

    if ((chunk = r_take(&r, ISO_MTI_LEN)) == NULL)
        goto format_error;
    memcpy(m->mti, chunk, ISO_MTI_LEN);
    m->mti[ISO_MTI_LEN] = '\0';
    if (!mti_valid(m->mti))
        goto format_error;

    if ((chunk = r_take(&r, 8)) == NULL)
        goto format_error;
    memcpy(bitmap, chunk, 8);
    if (bitmap_test(bitmap, 1)) {
        if ((chunk = r_take(&r, 8)) == NULL)
            goto format_error;
        memcpy(bitmap + 8, chunk, 8);
    }

    for (int f = 2; f <= ISO_MAX_FIELD; f++) {
        if (!bitmap_test(bitmap, f))
            continue;
        const iso_field_spec *sp = iso_spec(f);
        if (sp == NULL)
            goto format_error;
        size_t flen = sp->max_len;
        if (sp->len_type != ISO_FIXED) {
            size_t digits = sp->len_type == ISO_LLVAR ? 2 : 3;
            if ((chunk = r_take(&r, digits)) == NULL || !parse_digits(chunk, digits, &flen))
                goto format_error;
        }
        if ((chunk = r_take(&r, flen)) == NULL)
            goto format_error;
        if (validate(sp, (const char *)chunk, flen) != MP_OK)
            goto format_error;
        if ((m->fields[f] = dup_n((const char *)chunk, flen)) == NULL) {
            iso_msg_free(m);
            return MP_ERR_NOMEM;
        }
    }
    if (r.left != 0)
        goto format_error;
    return MP_OK;

format_error:
    iso_msg_free(m);
    return MP_ERR_FORMAT;
}

void iso_dump(const iso_msg *m, FILE *out)
{
    if (m == NULL || out == NULL)
        return;
    fprintf(out, "MTI %s\n", m->mti[0] ? m->mti : "(none)");
    for (int f = 2; f <= ISO_MAX_FIELD; f++) {
        const char *v = m->fields[f];
        if (v == NULL)
            continue;
        const iso_field_spec *sp = iso_spec(f);
        char masked[PAN_MAX_LEN + 1];
        if (f == F_PAN) {
            pan_mask(v, masked, sizeof masked);
            v = masked;
        }
        fprintf(out, "  [%3d] %-24s %s\n", f, sp ? sp->name : "?", v);
    }
}
