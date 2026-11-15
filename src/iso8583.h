#ifndef MP_ISO8583_H
#define MP_ISO8583_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "common.h"

#define ISO_MAX_FIELD 128
#define ISO_MTI_LEN 4
#define ISO_MAX_VALUE_LEN 999

enum {
    F_PAN = 2,
    F_PROC_CODE = 3,
    F_AMOUNT = 4,
    F_TRANSMISSION_DT = 7,
    F_STAN = 11,
    F_LOCAL_TIME = 12,
    F_LOCAL_DATE = 13,
    F_EXPIRY = 14,
    F_POS_ENTRY = 22,
    F_RRN = 37,
    F_AUTH_CODE = 38,
    F_RESP_CODE = 39,
    F_TERMINAL_ID = 41,
    F_MERCHANT_ID = 42,
    F_CURRENCY = 49,
    F_ADDL_AMOUNTS = 54,
    F_NETWORK_CODE = 70,
    F_ORIGINAL_DATA = 90
};

typedef enum { ISO_FIXED, ISO_LLVAR, ISO_LLLVAR } iso_len_type;
typedef enum { ISO_N, ISO_AN, ISO_ANS } iso_charset;

typedef struct {
    const char *name;
    iso_len_type len_type;
    iso_charset charset;
    uint16_t max_len;
} iso_field_spec;

typedef struct {
    char mti[ISO_MTI_LEN + 1];
    char *fields[ISO_MAX_FIELD + 1];
} iso_msg;

void iso_msg_init(iso_msg *m);
void iso_msg_free(iso_msg *m);

mp_status iso_set_mti(iso_msg *m, const char *mti);
mp_status iso_set(iso_msg *m, int field, const char *value);
mp_status iso_setf(iso_msg *m, int field, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void iso_unset(iso_msg *m, int field);
const char *iso_get(const iso_msg *m, int field);
bool iso_has(const iso_msg *m, int field);
mp_status iso_copy_field(iso_msg *dst, const iso_msg *src, int field);

const iso_field_spec *iso_spec(int field);

mp_status iso_pack(const iso_msg *m, uint8_t *buf, size_t cap, size_t *out_len);

mp_status iso_unpack(const uint8_t *buf, size_t len, iso_msg *m);

void iso_dump(const iso_msg *m, FILE *out);

#endif
