#include "gw_conv.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "spb.h"

bool gw_conv_is_scaled(const gw_tag_cfg_t *tag)
{
    return tag->scale != 1.0 || tag->offset != 0.0;
}

static bool is_wide(gw_type_t type)
{
    return type == GW_TYPE_UINT64 || type == GW_TYPE_INT64 || type == GW_TYPE_FLOAT64;
}

gw_val_kind_t gw_conv_kind(const gw_tag_cfg_t *tag)
{
    if (tag->type == GW_TYPE_STRING) {
        return GW_VK_STRING;
    }
    if (tag->type == GW_TYPE_BYTES) {
        return GW_VK_BYTES;
    }
    if (!gw_conv_is_scaled(tag)) {
        if (tag->type == GW_TYPE_INT64) {
            return GW_VK_INT64;
        }
        if (tag->type == GW_TYPE_UINT64) {
            return GW_VK_UINT64;
        }
    }
    return GW_VK_DOUBLE;
}

uint32_t gw_conv_datatype(const gw_tag_cfg_t *tag)
{
    if (tag->type == GW_TYPE_BOOL) {
        return SPB_DT_BOOLEAN;
    }
    if (tag->type == GW_TYPE_STRING) {
        return SPB_DT_STRING;
    }
    if (tag->type == GW_TYPE_BYTES) {
        return SPB_DT_BYTES;
    }
    if (gw_conv_is_scaled(tag)) {
        /* Float keeps the MVP mapping; 64-bit sources need a Double to keep their precision */
        return is_wide(tag->type) ? SPB_DT_DOUBLE : SPB_DT_FLOAT;
    }
    switch (tag->type) {
    case GW_TYPE_INT16:
        return SPB_DT_INT16;
    case GW_TYPE_UINT16:
        return SPB_DT_UINT16;
    case GW_TYPE_INT32:
        return SPB_DT_INT32;
    case GW_TYPE_UINT32:
        return SPB_DT_UINT32;
    case GW_TYPE_INT64:
        return SPB_DT_INT64;
    case GW_TYPE_UINT64:
        return SPB_DT_UINT64;
    case GW_TYPE_FLOAT64:
        return SPB_DT_DOUBLE;
    default:
        return SPB_DT_FLOAT;
    }
}

/* ---------- byte order ---------- */

static bool byte_swapped(const gw_tag_cfg_t *tag)
{
    return tag->word_order == GW_WORD_BADC || tag->word_order == GW_WORD_DCBA;
}

static bool word_swapped(const gw_tag_cfg_t *tag)
{
    return tag->word_order == GW_WORD_LITTLE || tag->word_order == GW_WORD_DCBA;
}

static uint16_t swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

/* @p n registers -> integer, most significant word first after the tag's byte order. */
static uint64_t join(const gw_tag_cfg_t *tag, const uint16_t *regs, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        uint16_t r = regs[word_swapped(tag) ? n - 1u - i : i];
        v = (v << 16) | (byte_swapped(tag) ? swap16(r) : r);
    }
    return v;
}

static void split(const gw_tag_cfg_t *tag, uint64_t v, uint16_t *regs, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint16_t r = (uint16_t)(v >> (16u * (n - 1u - i)));   /* i-th most significant word */
        regs[word_swapped(tag) ? n - 1u - i : i] = byte_swapped(tag) ? swap16(r) : r;
    }
}

/* ---------- strings ---------- */

/* Two characters per register, the first in the high byte (in the low byte with BADC/DCBA; the
 * word order does not apply). Stops at the first NUL; trailing spaces (padding) are dropped and
 * anything outside printable ASCII becomes '?', so the Sparkplug String is always valid UTF-8. */
static void decode_string(const gw_tag_cfg_t *tag, const uint16_t *regs, char *out)
{
    size_t n = 0;
    for (unsigned i = 0; i < tag->length; i++) {
        uint16_t r = byte_swapped(tag) ? swap16(regs[i]) : regs[i];
        const uint8_t c[2] = {(uint8_t)(r >> 8), (uint8_t)r};
        for (int k = 0; k < 2; k++) {
            if (c[k] == 0) {
                goto done;
            }
            out[n++] = (c[k] >= 0x20 && c[k] <= 0x7E) ? (char)c[k] : '?';
        }
    }
done:
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
}

static gw_conv_status_t encode_string(const gw_tag_cfg_t *tag, const char *s, uint16_t *regs)
{
    size_t len = strnlen(s, GW_STR_MAX + 1u);
    if (len > 2u * tag->length) {
        return GW_CONV_RANGE;
    }
    for (unsigned i = 0; i < tag->length; i++) {
        uint8_t hi = 2u * i < len ? (uint8_t)s[2u * i] : 0;
        uint8_t lo = 2u * i + 1u < len ? (uint8_t)s[2u * i + 1u] : 0;
        uint16_t r = (uint16_t)((hi << 8) | lo);
        regs[i] = byte_swapped(tag) ? swap16(r) : r;
    }
    return GW_CONV_OK;
}

/* SPB-19 raw bytes: the bytes of each register as they arrive, high byte first (low byte first
 * with BADC/DCBA); no other processing. */
static void decode_bytes(const gw_tag_cfg_t *tag, const uint16_t *regs, char *out)
{
    for (unsigned i = 0; i < tag->length; i++) {
        uint16_t r = byte_swapped(tag) ? swap16(regs[i]) : regs[i];
        out[2u * i] = (char)(r >> 8);
        out[2u * i + 1u] = (char)(r & 0xFFu);
    }
    out[2u * tag->length] = '\0';
}

static void encode_bytes(const gw_tag_cfg_t *tag, const char *b, uint16_t *regs)
{
    for (unsigned i = 0; i < tag->length; i++) {
        uint16_t r = (uint16_t)(((uint8_t)b[2u * i] << 8) | (uint8_t)b[2u * i + 1u]);
        regs[i] = byte_swapped(tag) ? swap16(r) : r;
    }
}

/* ---------- decode ---------- */

void gw_conv_decode(const gw_tag_cfg_t *tag, const uint16_t *regs, gw_tag_value_t *out)
{
    double raw;
    switch (tag->type) {
    case GW_TYPE_STRING:
        decode_string(tag, regs, out->str);
        return;
    case GW_TYPE_BYTES:
        decode_bytes(tag, regs, out->str);
        return;
    case GW_TYPE_INT16:
        raw = (int16_t)join(tag, regs, 1);
        break;
    case GW_TYPE_UINT32:
        raw = (double)join(tag, regs, 2);
        break;
    case GW_TYPE_INT32:
        raw = (int32_t)(uint32_t)join(tag, regs, 2);
        break;
    case GW_TYPE_FLOAT32: {
        uint32_t bits = (uint32_t)join(tag, regs, 2);
        float f;
        memcpy(&f, &bits, sizeof(f));
        raw = f;
        break;
    }
    case GW_TYPE_UINT64: {
        uint64_t u = join(tag, regs, 4);
        if (gw_conv_kind(tag) == GW_VK_UINT64) {
            out->u64 = u;
            return;
        }
        raw = (double)u;
        break;
    }
    case GW_TYPE_INT64: {
        int64_t i = (int64_t)join(tag, regs, 4);
        if (gw_conv_kind(tag) == GW_VK_INT64) {
            out->i64 = i;
            return;
        }
        raw = (double)i;
        break;
    }
    case GW_TYPE_FLOAT64: {
        uint64_t bits = join(tag, regs, 4);
        memcpy(&raw, &bits, sizeof(raw));
        break;
    }
    default:
        raw = (double)join(tag, regs, 1);   /* uint16 */
        break;
    }
    out->value = raw * tag->scale + tag->offset;
}

double gw_conv_from_regs(const gw_tag_cfg_t *tag, const uint16_t *regs)
{
    gw_tag_value_t v;
    memset(&v, 0, sizeof(v));
    gw_conv_decode(tag, regs, &v);
    return gw_conv_as_double(tag, &v);
}

/* ---------- encode ---------- */

/* 2^63 and 2^64 as doubles: the first value out of range of int64 / uint64. */
#define TWO63 9223372036854775808.0
#define TWO64 18446744073709551616.0

gw_conv_status_t gw_conv_encode(const gw_tag_cfg_t *tag, const gw_tag_value_t *in, uint16_t *regs)
{
    switch (gw_conv_kind(tag)) {
    case GW_VK_STRING:
        return encode_string(tag, in->str, regs);
    case GW_VK_BYTES:
        encode_bytes(tag, in->str, regs);
        return GW_CONV_OK;
    case GW_VK_INT64:
        split(tag, (uint64_t)in->i64, regs, 4);
        return GW_CONV_OK;
    case GW_VK_UINT64:
        split(tag, in->u64, regs, 4);
        return GW_CONV_OK;
    default:
        break;
    }
    double value = in->value;
    if (!isfinite(value)) {
        return GW_CONV_NOT_FINITE;
    }
    double raw = (value - tag->offset) / tag->scale;
    if (tag->type == GW_TYPE_FLOAT32) {
        if (fabs(raw) > 3.4028234663852886e38) {
            return GW_CONV_RANGE;
        }
        float f = (float)raw;
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        split(tag, bits, regs, 2);
        return GW_CONV_OK;
    }
    if (tag->type == GW_TYPE_FLOAT64) {
        if (!isfinite(raw)) {
            return GW_CONV_RANGE;
        }
        uint64_t bits;
        memcpy(&bits, &raw, sizeof(bits));
        split(tag, bits, regs, 4);
        return GW_CONV_OK;
    }
    double r = round(raw);
    switch (tag->type) {
    case GW_TYPE_INT16:
        if (r < -32768.0 || r > 32767.0) {
            return GW_CONV_RANGE;
        }
        split(tag, (uint16_t)(int16_t)r, regs, 1);
        return GW_CONV_OK;
    case GW_TYPE_UINT16:
        if (r < 0.0 || r > 65535.0) {
            return GW_CONV_RANGE;
        }
        split(tag, (uint16_t)r, regs, 1);
        return GW_CONV_OK;
    case GW_TYPE_INT32:
        if (r < -2147483648.0 || r > 2147483647.0) {
            return GW_CONV_RANGE;
        }
        split(tag, (uint32_t)(int32_t)r, regs, 2);
        return GW_CONV_OK;
    case GW_TYPE_UINT32:
        if (r < 0.0 || r > 4294967295.0) {
            return GW_CONV_RANGE;
        }
        split(tag, (uint32_t)r, regs, 2);
        return GW_CONV_OK;
    case GW_TYPE_INT64:   /* scaled: from a double */
        if (r < -TWO63 || r >= TWO63) {
            return GW_CONV_RANGE;
        }
        split(tag, (uint64_t)(int64_t)r, regs, 4);
        return GW_CONV_OK;
    case GW_TYPE_UINT64:
        if (r < 0.0 || r >= TWO64) {
            return GW_CONV_RANGE;
        }
        split(tag, (uint64_t)r, regs, 4);
        return GW_CONV_OK;
    default:
        return GW_CONV_RANGE;  /* bool tags are written as coils or register bits */
    }
}

bool gw_conv_from_double(const gw_tag_cfg_t *tag, double value, gw_tag_value_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (gw_conv_kind(tag)) {
    case GW_VK_STRING:
    case GW_VK_BYTES:
        return false;
    case GW_VK_INT64:
        if (!isfinite(value) || value != floor(value) || value < -TWO63 || value >= TWO63) {
            return false;
        }
        out->i64 = (int64_t)value;
        return true;
    case GW_VK_UINT64:
        if (!isfinite(value) || value != floor(value) || value < 0.0 || value >= TWO64) {
            return false;
        }
        out->u64 = (uint64_t)value;
        return true;
    default:
        out->value = value;
        return true;
    }
}

gw_conv_status_t gw_conv_to_regs(const gw_tag_cfg_t *tag, double value, uint16_t *regs)
{
    if (!isfinite(value)) {
        return GW_CONV_NOT_FINITE;
    }
    gw_tag_value_t v;
    if (!gw_conv_from_double(tag, value, &v)) {
        return GW_CONV_RANGE;
    }
    return gw_conv_encode(tag, &v, regs);
}

double gw_conv_as_double(const gw_tag_cfg_t *tag, const gw_tag_value_t *v)
{
    switch (gw_conv_kind(tag)) {
    case GW_VK_INT64:
        return (double)v->i64;
    case GW_VK_UINT64:
        return (double)v->u64;
    case GW_VK_STRING:
    case GW_VK_BYTES:
        return 0.0;
    default:
        return v->value;
    }
}

void gw_conv_format(const gw_tag_cfg_t *tag, const gw_tag_value_t *v, char *buf, size_t cap)
{
    switch (gw_conv_kind(tag)) {
    case GW_VK_INT64:
        snprintf(buf, cap, "%" PRId64, v->i64);
        break;
    case GW_VK_UINT64:
        snprintf(buf, cap, "%" PRIu64, v->u64);
        break;
    case GW_VK_STRING:
        snprintf(buf, cap, "'%s'", v->str);
        break;
    case GW_VK_BYTES: {   /* hex, two digits per byte */
        size_t n = 0;
        for (unsigned i = 0; i < 2u * tag->length && n + 3 <= cap; i++) {
            n += (size_t)snprintf(buf + n, cap - n, "%02x", (uint8_t)v->str[i]);
        }
        if (cap > 0) {
            buf[n < cap ? n : cap - 1] = '\0';
        }
        break;
    }
    default:
        snprintf(buf, cap, "%.10g", v->value);
        break;
    }
}
