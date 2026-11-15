#ifndef MP_LUHN_H
#define MP_LUHN_H

#include <stdbool.h>
#include <stddef.h>

#define PAN_MIN_LEN 12
#define PAN_MAX_LEN 19

bool luhn_valid(const char *pan);

char luhn_check_digit(const char *partial);

void pan_mask(const char *pan, char *out, size_t out_size);

#endif
