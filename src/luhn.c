#include "luhn.h"

#include <string.h>

static int luhn_sum(const char *digits, size_t len, int double_first)
{
    int sum = 0;
    int dbl = double_first;
    for (size_t i = len; i-- > 0;) {
        int d = digits[i] - '0';
        if (dbl) {
            d *= 2;
            if (d > 9)
                d -= 9;
        }
        sum += d;
        dbl = !dbl;
    }
    return sum;
}

static int all_digits(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (s[i] < '0' || s[i] > '9')
            return 0;
    return 1;
}

bool luhn_valid(const char *pan)
{
    if (pan == NULL)
        return false;
    size_t len = strlen(pan);
    if (len < PAN_MIN_LEN || len > PAN_MAX_LEN || !all_digits(pan, len))
        return false;
    return luhn_sum(pan, len, 0) % 10 == 0;
}

char luhn_check_digit(const char *partial)
{
    if (partial == NULL)
        return '\0';
    size_t len = strlen(partial);
    if (len == 0 || !all_digits(partial, len))
        return '\0';
    int sum = luhn_sum(partial, len, 1);
    return (char)('0' + (10 - sum % 10) % 10);
}

void pan_mask(const char *pan, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0)
        return;
    if (pan == NULL) {
        out[0] = '\0';
        return;
    }
    size_t len = strlen(pan);
    size_t i = 0;
    for (; i < len && i + 1 < out_size; i++) {
        int visible = len >= 10 && (i < 6 || i >= len - 4);
        out[i] = visible ? pan[i] : '*';
    }
    out[i] = '\0';
}
