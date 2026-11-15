#ifndef MP_COMMON_H
#define MP_COMMON_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    MP_OK = 0,
    MP_ERR_ARG = -1,
    MP_ERR_NOMEM = -2,
    MP_ERR_FORMAT = -3,
    MP_ERR_OVERFLOW = -4,
    MP_ERR_NOTFOUND = -5,
    MP_ERR_IO = -6,
    MP_ERR_CLOSED = -7,
    MP_ERR_EXISTS = -8,
    MP_ERR_TIMEOUT = -9
} mp_status;

const char *mp_strerror(mp_status st);

#define MP_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif
