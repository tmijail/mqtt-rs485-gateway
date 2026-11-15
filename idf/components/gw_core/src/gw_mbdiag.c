#include "gw_mbdiag.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gw_config_priv.h"
#include "gw_conv.h"
#include "gw_port.h"

static const char *const k_area_text[] = {"coil", "discrete", "holding", "input"};

/* An integer key in [min, max]; @p def when absent and not required. */
static bool get_uint(const cJSON *root, const char *key, bool required, long def, long min,
                     long max, long *out, char *why, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (v == NULL) {
        if (required) {
            snprintf(why, cap, "\"%s\" is missing", key);
            return false;
        }
        *out = def;
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != floor(v->valuedouble) || v->valuedouble < (double)min ||
        v->valuedouble > (double)max) {
        snprintf(why, cap, "\"%s\" must be an integer %ld-%ld", key, min, max);
        return false;
    }
    *out = (long)v->valuedouble;
    return true;
}

/* A string key; NULL when absent. False (with the reason) if it is not a string. */
static bool get_text(const cJSON *root, const char *key, const char **out, char *why, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    *out = NULL;
    if (v == NULL) {
        return true;
    }
    if (!cJSON_IsString(v)) {
        snprintf(why, cap, "\"%s\" must be a string", key);
        return false;
    }
    *out = v->valuestring;
    return true;
}

static bool parse_root(const cJSON *root, gw_mbdiag_req_t *rq, char *why, size_t cap)
{
    static const char *const keys[] = {"unit", "area", "address", "count", "type", "word_order"};
    if (!cJSON_IsObject(root)) {
        snprintf(why, cap, "expected a JSON object {\"unit\": 1, \"area\": \"holding\", "
                           "\"address\": 0, \"count\": 1}");
        return false;
    }
    for (const cJSON *it = root->child; it != NULL; it = it->next) {
        bool known = false;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            known = known || strcmp(it->string, keys[i]) == 0;
        }
        if (!known) {
            snprintf(why, cap, "unknown key \"%.32s\"", it->string);
            return false;
        }
    }
    long unit, addr, count;
    const char *area, *type, *word;
    if (!get_uint(root, "unit", true, 0, 1, 247, &unit, why, cap) ||
        !get_text(root, "area", &area, why, cap) ||
        !get_uint(root, "address", true, 0, 0, 65535, &addr, why, cap) ||
        !get_text(root, "type", &type, why, cap) || !get_text(root, "word_order", &word, why, cap)) {
        return false;
    }
    memset(rq, 0, sizeof(*rq));
    if (area == NULL || !gw_config_area_of(area, &rq->area)) {
        snprintf(why, cap, "\"area\" must be coil, discrete, holding or input");
        return false;
    }
    bool bits = rq->area == GW_AREA_COIL || rq->area == GW_AREA_DISCRETE;
    rq->tag.scale = 1.0;
    rq->tag.bit = GW_NO_BIT;
    rq->tag.area = rq->area;
    rq->tag.word_order = GW_WORD_BIG;
    rq->convert = type != NULL;
    if (word != NULL && type == NULL) {
        snprintf(why, cap, "\"word_order\" needs a \"type\"");
        return false;
    }
    if (type != NULL && !gw_config_type_of(type, &rq->tag.type)) {
        snprintf(why, cap, "\"type\" must be a tag type (bool, uint16, int16, uint32, int32, "
                           "float32, uint64, int64, float64, string, bytes)");
        return false;
    }
    if (word != NULL && !gw_config_word_order_of(word, &rq->tag.word_order)) {
        snprintf(why, cap, "\"word_order\" must be big, little, ABCD, CDAB, BADC or DCBA");
        return false;
    }
    if (rq->convert && bits != (rq->tag.type == GW_TYPE_BOOL)) {
        snprintf(why, cap, bits ? "the %s area holds bits: type bool only"
                                : "type bool is for the coil and discrete areas; read the "
                                  "register as uint16 to see its bits",
                 area);
        return false;
    }
    bool blob = rq->tag.type == GW_TYPE_STRING || rq->tag.type == GW_TYPE_BYTES;
    long width = !rq->convert || blob ? 1 : (long)gw_tag_width(&rq->tag);
    if (!get_uint(root, "count", false, width, 1, GW_MBDIAG_MAX, &count, why, cap)) {
        return false;
    }
    if (rq->convert && count < width) {
        snprintf(why, cap, "\"count\" %ld is less than the %ld registers of %s", count, width,
                 type);
        return false;
    }
    if (addr + count > 65536) {
        snprintf(why, cap, "address %ld + count %ld is past 65535", addr, count);
        return false;
    }
    rq->unit = (uint8_t)unit;
    rq->address = (uint16_t)addr;
    rq->count = (uint16_t)count;
    rq->tag.address = (uint16_t)addr;
    rq->tag.length = blob ? (uint8_t)count : 0;
    return true;
}

bool gw_mbdiag_parse(const char *json, size_t len, gw_mbdiag_req_t *rq, char *why, size_t cap)
{
    gw_port_json_lock();
    gw_config_json_hooks();
    cJSON *root = cJSON_ParseWithLength(json, len);
    bool ok;
    if (root == NULL) {
        snprintf(why, cap, gw_config_json_alloc_failed() ? "out of memory parsing the request"
                                                         : "the request is not valid JSON");
        ok = false;
    } else {
        ok = parse_root(root, rq, why, cap);
    }
    cJSON_Delete(root);
    gw_port_json_unlock();
    return ok;
}

mb_fc_t gw_mbdiag_fc(const gw_mbdiag_req_t *rq)
{
    static const mb_fc_t fcs[] = {MB_FC_READ_COILS, MB_FC_READ_DISCRETE_INPUTS,
                                  MB_FC_READ_HOLDING_REGISTERS, MB_FC_READ_INPUT_REGISTERS};
    return fcs[rq->area];
}

/* Appends to a bounded buffer; the text is cut, never overrun. */
typedef struct {
    char *buf;
    size_t cap, len;
} sink_t;

static void put(sink_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(sink_t *s, const char *fmt, ...)
{
    if (s->len + 1 >= s->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        s->len += (size_t)n < s->cap - s->len ? (size_t)n : s->cap - s->len - 1;
    }
}

static const char *exception_text(uint8_t code)
{
    switch (code) {
    case 1:
        return "illegal function";
    case 2:
        return "illegal data address";
    case 3:
        return "illegal data value";
    case 4:
        return "server device failure";
    case 5:
        return "acknowledge";
    case 6:
        return "server device busy";
    case 8:
        return "memory parity error";
    case 10:
        return "gateway path unavailable";
    case 11:
        return "gateway target device failed to respond";
    default:
        return "unknown exception";
    }
}

static void put_value(sink_t *s, const gw_mbdiag_req_t *rq, const uint16_t *values)
{
    static const char *const words[] = {"ABCD", "CDAB", "BADC", "DCBA"};   /* gw_word_order_t */
    put(s, ",\"type\":\"%s\",\"word_order\":\"%s\",\"value\":",
        gw_config_type_name(rq->tag.type), words[rq->tag.word_order]);
    if (rq->tag.type == GW_TYPE_BOOL) {
        put(s, "%s", values[0] ? "true" : "false");
        return;
    }
    gw_tag_value_t v;
    memset(&v, 0, sizeof(v));
    gw_conv_decode(&rq->tag, values, &v);
    switch (gw_conv_kind(&rq->tag)) {
    case GW_VK_INT64:   /* #188: also as text, for a JSON reader with doubles (a browser) */
        put(s, "%lld,\"value_text\":\"%lld\"", (long long)v.i64, (long long)v.i64);
        break;
    case GW_VK_UINT64:
        put(s, "%llu,\"value_text\":\"%llu\"", (unsigned long long)v.u64,
            (unsigned long long)v.u64);
        break;
    case GW_VK_STRING:   /* printable ASCII (gw_conv): only " and \ need an escape */
        put(s, "\"");
        for (const char *c = v.str; *c; c++) {
            put(s, (*c == '"' || *c == '\\') ? "\\%c" : "%c", *c);
        }
        put(s, "\"");
        break;
    case GW_VK_BYTES: {
        char hex[2 * GW_STR_MAX + 1];
        gw_conv_format(&rq->tag, &v, hex, sizeof(hex));
        put(s, "\"%s\"", hex);
        break;
    }
    default:
        if (isfinite(v.value)) {   /* NaN and infinity are not JSON */
            put(s, rq->tag.type == GW_TYPE_FLOAT64 ? "%.17g" : "%.9g", v.value);
        } else {
            put(s, "null");
        }
        break;
    }
}

const char *gw_mbdiag_answer(const gw_mbdiag_req_t *rq, mb_status_t st, uint8_t exception,
                             const uint16_t *values, char *out, size_t cap)
{
    sink_t s = {out, cap, 0};
    if (cap > 0) {
        out[0] = '\0';
    }
    const char *status;
    switch (st) {
    case MB_OK:
        put(&s, "{\"ok\":true,\"unit\":%u,\"area\":\"%s\",\"address\":%u,\"count\":%u,\"values\":[",
            rq->unit, k_area_text[rq->area], rq->address, rq->count);
        for (uint16_t i = 0; i < rq->count; i++) {
            put(&s, "%s%u", i ? "," : "", values[i]);
        }
        put(&s, "]");
        if (rq->convert) {
            put_value(&s, rq, values);
        }
        put(&s, "}\n");
        return "200 OK";
    case MB_ERR_TIMEOUT:
        put(&s, "{\"ok\":false,\"result\":\"timeout\",\"error\":\"no answer from unit %u "
                "(timeout)\"}\n", rq->unit);
        return "504 Gateway Timeout";
    case MB_ERR_CRC:
        put(&s, "{\"ok\":false,\"result\":\"crc\",\"error\":\"unit %u: CRC error in the "
                "answer\"}\n", rq->unit);
        status = "502 Bad Gateway";
        break;
    case MB_ERR_FRAME:
        put(&s, "{\"ok\":false,\"result\":\"frame\",\"error\":\"unit %u: framing error in the "
                "answer\"}\n", rq->unit);
        status = "502 Bad Gateway";
        break;
    case MB_ERR_EXCEPTION:
        put(&s, "{\"ok\":false,\"result\":\"exception\",\"exception\":%u,\"error\":\"unit %u: "
                "Modbus exception %u (%s)\"}\n", exception, rq->unit, exception,
            exception_text(exception));
        status = "502 Bad Gateway";
        break;
    default:
        put(&s, "{\"ok\":false,\"result\":\"busy\",\"error\":\"the bus did not run the read in "
                "time, or another test read is pending: try again\"}\n");
        status = "503 Service Unavailable";
        break;
    }
    return status;
}
