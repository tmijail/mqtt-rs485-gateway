#include "spb.h"

#include <stdio.h>
#include <string.h>

#include "pb_decode.h"
#include "pb_encode.h"
#include "sparkplug_b.pb.h"

_Static_assert(sizeof(((Payload_Metric *)0)->name) == SPB_NAME_MAX + 1, "nanopb options drift");
_Static_assert(sizeof(((Payload_Metric *)0)->value.string_value) == SPB_STRING_MAX + 1,
               "nanopb options drift");

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void fill_metric(Payload_Metric *out, const spb_metric_t *m)
{
    *out = (Payload_Metric)Payload_Metric_init_zero;
    if (m->name != NULL) {
        out->has_name = true;
        copy_str(out->name, sizeof(out->name), m->name);
    }
    out->has_alias = true;
    out->alias = m->alias;
    out->has_datatype = true;
    out->datatype = m->datatype;
    if (m->timestamp != 0) {
        out->has_timestamp = true;
        out->timestamp = m->timestamp;
    }
    if (m->is_historical) {
        out->has_is_historical = true;
        out->is_historical = true;
    }
    if (m->is_null) {
        out->has_is_null = true;
        out->is_null = true;
        return;
    }
    double v = m->v.num;
    switch (m->datatype) {
    case SPB_DT_INT8:
    case SPB_DT_INT16:
    case SPB_DT_INT32:
        out->which_value = Payload_Metric_int_value_tag;
        out->value.int_value = (uint32_t)(int32_t)v;  /* two's complement, sign-extended */
        break;
    case SPB_DT_UINT8:
    case SPB_DT_UINT16:
    case SPB_DT_UINT32:
        out->which_value = Payload_Metric_int_value_tag;
        out->value.int_value = (uint32_t)v;
        break;
    case SPB_DT_INT64:
        out->which_value = Payload_Metric_long_value_tag;
        out->value.long_value = (uint64_t)m->v.i64;   /* two's complement */
        break;
    case SPB_DT_UINT64:
    case SPB_DT_DATETIME:
        out->which_value = Payload_Metric_long_value_tag;
        out->value.long_value = m->v.u64;
        break;
    case SPB_DT_FLOAT:
        out->which_value = Payload_Metric_float_value_tag;
        out->value.float_value = (float)v;
        break;
    case SPB_DT_DOUBLE:
        out->which_value = Payload_Metric_double_value_tag;
        out->value.double_value = v;
        break;
    case SPB_DT_BOOLEAN:
        out->which_value = Payload_Metric_boolean_value_tag;
        out->value.boolean_value = v != 0.0;
        break;
    case SPB_DT_STRING:
    case SPB_DT_TEXT:
        out->which_value = Payload_Metric_string_value_tag;
        copy_str(out->value.string_value, sizeof(out->value.string_value),
                 m->v.str != NULL ? m->v.str : "");
        break;
    default:
        out->has_is_null = true;
        out->is_null = true;
        break;
    }
}

static bool enc_metrics(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    const spb_payload_t *p = *arg;
    Payload_Metric m;
    for (size_t i = 0; i < p->metric_count; i++) {
        fill_metric(&m, &p->metrics[i]);
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, Payload_Metric_fields, &m)) {
            return false;
        }
    }
    return true;
}

size_t spb_encode(const spb_payload_t *p, uint8_t *buf, size_t cap)
{
    Payload msg = Payload_init_zero;
    msg.has_timestamp = true;
    msg.timestamp = p->timestamp;
    msg.has_seq = p->has_seq;
    msg.seq = p->seq;
    msg.metrics.funcs.encode = enc_metrics;
    msg.metrics.arg = (void *)p;

    pb_ostream_t os = pb_ostream_from_buffer(buf, cap);
    if (!pb_encode(&os, Payload_fields, &msg)) {
        return 0;
    }
    return os.bytes_written;
}

typedef struct {
    spb_metric_cb_t cb;
    void *ctx;
} dec_ctx_t;

static bool dec_metric(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    const dec_ctx_t *dc = *arg;
    Payload_Metric m = Payload_Metric_init_zero;
    if (!pb_decode(stream, Payload_Metric_fields, &m)) {
        return false;
    }
    spb_rx_metric_t rx = {
        .name = m.has_name ? m.name : NULL,
        .has_alias = m.has_alias,
        .alias = m.alias,
        .has_datatype = m.has_datatype,
        .datatype = m.datatype,
        .timestamp = m.has_timestamp ? m.timestamp : 0,
        .is_null = m.has_is_null && m.is_null,
        .is_historical = m.has_is_historical && m.is_historical,
    };
    switch (m.which_value) {
    case Payload_Metric_int_value_tag:
        rx.kind = SPB_VAL_INT;
        rx.v.i = m.value.int_value;
        break;
    case Payload_Metric_long_value_tag:
        rx.kind = SPB_VAL_LONG;
        rx.v.l = m.value.long_value;
        break;
    case Payload_Metric_float_value_tag:
        rx.kind = SPB_VAL_FLOAT;
        rx.v.f = m.value.float_value;
        break;
    case Payload_Metric_double_value_tag:
        rx.kind = SPB_VAL_DOUBLE;
        rx.v.d = m.value.double_value;
        break;
    case Payload_Metric_boolean_value_tag:
        rx.kind = SPB_VAL_BOOL;
        rx.v.b = m.value.boolean_value;
        break;
    case Payload_Metric_string_value_tag:
        rx.kind = SPB_VAL_STRING;
        rx.v.s = m.value.string_value;
        break;
    default:
        rx.kind = SPB_VAL_NONE;
        break;
    }
    return dc->cb != NULL ? dc->cb(dc->ctx, &rx) : true;
}

bool spb_decode(const uint8_t *buf, size_t len, spb_rx_header_t *hdr, spb_metric_cb_t cb,
                void *ctx)
{
    dec_ctx_t dc = {.cb = cb, .ctx = ctx};
    Payload msg = Payload_init_zero;
    msg.metrics.funcs.decode = dec_metric;
    msg.metrics.arg = &dc;

    pb_istream_t is = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&is, Payload_fields, &msg)) {
        return false;
    }
    if (hdr != NULL) {
        hdr->has_timestamp = msg.has_timestamp;
        hdr->timestamp = msg.timestamp;
        hdr->has_seq = msg.has_seq;
        hdr->seq = msg.seq;
    }
    return true;
}

bool spb_rx_number(const spb_rx_metric_t *m, uint32_t datatype, double *out)
{
    if (m->is_null) {
        return false;
    }
    switch (m->kind) {
    case SPB_VAL_INT:
        switch (datatype) {
        case SPB_DT_INT8:
            *out = (int8_t)(uint8_t)m->v.i;
            break;
        case SPB_DT_INT16:
            *out = (int16_t)(uint16_t)m->v.i;
            break;
        case SPB_DT_INT32:
            *out = (int32_t)m->v.i;
            break;
        default:
            *out = m->v.i;
            break;
        }
        return true;
    case SPB_VAL_LONG:
        *out = datatype == SPB_DT_INT64 ? (double)(int64_t)m->v.l : (double)m->v.l;
        return true;
    case SPB_VAL_FLOAT:
        *out = m->v.f;
        return true;
    case SPB_VAL_DOUBLE:
        *out = m->v.d;
        return true;
    case SPB_VAL_BOOL:
        *out = m->v.b ? 1.0 : 0.0;
        return true;
    default:
        return false;
    }
}

bool spb_topic(char *out, size_t cap, const char *group, const char *type, const char *edge,
               const char *device)
{
    int n = device != NULL ? snprintf(out, cap, "spBv1.0/%s/%s/%s/%s", group, type, edge, device)
                           : snprintf(out, cap, "spBv1.0/%s/%s/%s", group, type, edge);
    return n > 0 && (size_t)n < cap;
}
