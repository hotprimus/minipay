#include "common.h"

const char *mp_strerror(mp_status st)
{
    switch (st) {
    case MP_OK:           return "ok";
    case MP_ERR_ARG:      return "invalid argument";
    case MP_ERR_NOMEM:    return "out of memory";
    case MP_ERR_FORMAT:   return "malformed data";
    case MP_ERR_OVERFLOW: return "buffer too small";
    case MP_ERR_NOTFOUND: return "not found";
    case MP_ERR_IO:       return "i/o error";
    case MP_ERR_CLOSED:   return "closed";
    case MP_ERR_EXISTS:   return "already exists";
    case MP_ERR_TIMEOUT:  return "timeout";
    }
    return "unknown error";
}
