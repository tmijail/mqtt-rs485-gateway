#include "gw_utf8.h"

#include <string.h>

size_t gw_utf8_seq(const char *s, size_t n)
{
    const unsigned char *c = (const unsigned char *)s;
    if (n == 0) {
        return 0;
    }
    if (c[0] < 0x80) {
        return 1;
    }
    size_t len;
    unsigned char lo = 0x80, hi = 0xbf;   /* bounds of the second byte (RFC 3629 table) */
    if (c[0] >= 0xc2 && c[0] <= 0xdf) {
        len = 2;
    } else if (c[0] >= 0xe0 && c[0] <= 0xef) {
        len = 3;
        lo = c[0] == 0xe0 ? 0xa0 : 0x80;   /* no overlong form */
        hi = c[0] == 0xed ? 0x9f : 0xbf;   /* no surrogate (U+D800-U+DFFF) */
    } else if (c[0] >= 0xf0 && c[0] <= 0xf4) {
        len = 4;
        lo = c[0] == 0xf0 ? 0x90 : 0x80;   /* no overlong form */
        hi = c[0] == 0xf4 ? 0x8f : 0xbf;   /* nothing above U+10FFFF */
    } else {
        return 0;   /* a continuation byte, an overlong lead (C0, C1) or F5-FF */
    }
    if (n < len || c[1] < lo || c[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < len; i++) {
        if (c[i] < 0x80 || c[i] > 0xbf) {
            return 0;
        }
    }
    return len;
}

bool gw_utf8_valid(const char *s)
{
    size_t n = strlen(s);
    while (n > 0) {
        size_t k = gw_utf8_seq(s, n);
        if (k == 0) {
            return false;
        }
        s += k;
        n -= k;
    }
    return true;
}

unsigned long gw_utf8_code(const char *s, size_t len)
{
    const unsigned char *c = (const unsigned char *)s;
    static const unsigned char lead_mask[5] = {0, 0x7f, 0x1f, 0x0f, 0x07};
    if (len == 0 || len > 4) {
        return 0;
    }
    unsigned long cp = c[0] & lead_mask[len];
    for (size_t i = 1; i < len; i++) {
        cp = (cp << 6) | (c[i] & 0x3fu);
    }
    return cp;
}
