#include "spb.h"

#include <stdio.h>
#include <string.h>

#include "gw_port.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "sparkplug_b.pb.h"

_Static_assert(sizeof(((Payload_Metric *)0)->name) == SPB_NAME_MAX + 1, "nanopb options drift");
_Static_assert(sizeof(((Payload_Metric *)0)->value.string_value) == SPB_STRING_MAX + 1,
               "nanopb options drift");

_Static_assert(sizeof(((Payload_PropertyValue *)0)->value.string_value) == SPB_SHORT_STRING_MAX + 1,
               "nanopb options drift");
_Static_assert(sizeof(((Payload_DataSet_DataSetValue *)0)->value.string_value) ==
                   SPB_SHORT_STRING_MAX + 1,
               "nanopb options drift");
_Static_assert(sizeof(((Payload_Template_Parameter *)0)->name) == SPB_NAME_MAX + 1,
               "nanopb options drift");
_Static_assert(sizeof(((Payload_Template_Parameter *)0)->value.string_value) ==
                   SPB_SHORT_STRING_MAX + 1,
               "nanopb options drift");

/* nanopb's messages are string literals: the pointer is enough */
static const char *s_last_error = "";

const char *spb_last_error(void)
{
    return s_last_error;
}

/* Issue #87: a string longer than its field is refused (the encoding fails, with a log), never
 * cut. Returns false then. */
static bool copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        GW_LOGE("spb", "string of %u characters does not fit (at most %u): '%.24s...' refused",
                (unsigned)n, (unsigned)(cap - 1), src);
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, src, n + 1);
    return true;
}

/* ---------- SPB-16 properties ---------- */

/* The properties of @p p that are present, in a fixed order: keys[] of them, and on request
 * the value of property @p want (keys and values are encoded by two callbacks that must agree;
 * one PropertyValue at a time keeps the encoder's stack small). */
enum { PROP_UNIT, PROP_LOW, PROP_HIGH, PROP_QUALITY, PROP_MAX };
static const char *const k_prop_keys[PROP_MAX] = {"engUnit", "engLow", "engHigh", "Quality"};

static size_t list_props(const spb_props_t *p, uint8_t ids[PROP_MAX])
{
    size_t n = 0;
    if (p->eng_unit != NULL && p->eng_unit[0] != '\0') {
        ids[n++] = PROP_UNIT;
    }
    if (p->has_eng_low) {
        ids[n++] = PROP_LOW;
    }
    if (p->has_eng_high) {
        ids[n++] = PROP_HIGH;
    }
    if (p->has_quality) {
        ids[n++] = PROP_QUALITY;
    }
    return n;
}

static void prop_value(const spb_props_t *p, uint8_t id, Payload_PropertyValue *pv)
{
    *pv = (Payload_PropertyValue)Payload_PropertyValue_init_zero;
    pv->has_type = true;
    switch (id) {
    case PROP_UNIT:
        pv->type = SPB_DT_STRING;
        pv->which_value = Payload_PropertyValue_string_value_tag;
        /* engUnit fits: the configuration bounds it (GW_UNITS_SIZE) */
        (void)copy_str(pv->value.string_value, sizeof(pv->value.string_value), p->eng_unit);
        break;
    case PROP_LOW:
    case PROP_HIGH:
        pv->type = SPB_DT_FLOAT;
        pv->which_value = Payload_PropertyValue_float_value_tag;
        pv->value.float_value = id == PROP_LOW ? p->eng_low : p->eng_high;
        break;
    default:
        pv->type = SPB_DT_INT32;
        pv->which_value = Payload_PropertyValue_int_value_tag;
        pv->value.int_value = (uint32_t)p->quality;
        break;
    }
}

static bool enc_prop_keys(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    uint8_t ids[PROP_MAX];
    size_t n = list_props(*arg, ids);
    for (size_t i = 0; i < n; i++) {
        const char *key = k_prop_keys[ids[i]];
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_string(stream, (const pb_byte_t *)key, strlen(key))) {
            return false;
        }
    }
    return true;
}

static bool enc_prop_values(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    uint8_t ids[PROP_MAX];
    size_t n = list_props(*arg, ids);
    for (size_t i = 0; i < n; i++) {
        Payload_PropertyValue pv;
        prop_value(*arg, ids[i], &pv);
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, Payload_PropertyValue_fields, &pv)) {
            return false;
        }
    }
    return true;
}

static bool enc_props(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    const spb_props_t *p = *arg;
    uint8_t ids[PROP_MAX];
    if (list_props(p, ids) == 0) {
        return true;   /* no PropertySet at all */
    }
    Payload_PropertySet ps = Payload_PropertySet_init_zero;
    ps.keys.funcs.encode = enc_prop_keys;
    ps.keys.arg = (void *)p;
    ps.values.funcs.encode = enc_prop_values;
    ps.values.arg = (void *)p;
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_submessage(stream, Payload_PropertySet_fields, &ps);
}

/* ---------- values ---------- */

/* The wire field of a scalar value, shared by metrics and DataSet cells. */
typedef enum { W_NONE, W_INT, W_LONG, W_FLOAT, W_DOUBLE, W_BOOL, W_STRING } wire_t;

typedef struct {
    wire_t wire;
    uint32_t i;
    uint64_t l;
    float f;
    double d;
    bool b;
    const char *s;
} scalar_t;

static scalar_t scalar(uint32_t datatype, const spb_value_t *v)
{
    scalar_t o = {.wire = W_NONE};
    switch (datatype) {
    case SPB_DT_INT8:
    case SPB_DT_INT16:
    case SPB_DT_INT32:
        o.wire = W_INT;
        o.i = (uint32_t)(int32_t)v->num;   /* two's complement, sign-extended */
        break;
    case SPB_DT_UINT8:
    case SPB_DT_UINT16:
    case SPB_DT_UINT32:
        o.wire = W_INT;
        o.i = (uint32_t)v->num;
        break;
    case SPB_DT_INT64:
        o.wire = W_LONG;
        o.l = (uint64_t)v->i64;            /* two's complement */
        break;
    case SPB_DT_UINT64:
    case SPB_DT_DATETIME:
        o.wire = W_LONG;
        o.l = v->u64;
        break;
    case SPB_DT_FLOAT:
        o.wire = W_FLOAT;
        o.f = (float)v->num;
        break;
    case SPB_DT_DOUBLE:
        o.wire = W_DOUBLE;
        o.d = v->num;
        break;
    case SPB_DT_BOOLEAN:
        o.wire = W_BOOL;
        o.b = v->num != 0.0;
        break;
    case SPB_DT_STRING:
    case SPB_DT_TEXT:
        o.wire = W_STRING;
        o.s = v->str != NULL ? v->str : "";
        break;
    default:
        break;
    }
    return o;
}

/* ---------- SPB-19 DataSet ---------- */

static bool enc_ds_columns(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    const spb_dataset_t *ds = *arg;
    for (uint32_t c = 0; c < ds->columns; c++) {
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_string(stream, (const pb_byte_t *)ds->names[c], strlen(ds->names[c]))) {
            return false;
        }
    }
    return true;
}

static bool enc_ds_types(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    const spb_dataset_t *ds = *arg;
    for (uint32_t c = 0; c < ds->columns; c++) {
        if (!pb_encode_tag_for_field(stream, field) || !pb_encode_varint(stream, ds->types[c])) {
            return false;
        }
    }
    return true;
}

typedef struct {
    const spb_dataset_t *ds;
    uint32_t row;
} ds_row_t;

static bool enc_ds_elements(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    const ds_row_t *r = *arg;
    for (uint32_t c = 0; c < r->ds->columns; c++) {
        spb_value_t v;
        memset(&v, 0, sizeof(v));
        r->ds->cell(r->ds->ctx, r->row, c, &v);
        scalar_t sc = scalar(r->ds->types[c], &v);
        Payload_DataSet_DataSetValue e = Payload_DataSet_DataSetValue_init_zero;
        switch (sc.wire) {
        case W_INT:
            e.which_value = Payload_DataSet_DataSetValue_int_value_tag;
            e.value.int_value = sc.i;
            break;
        case W_LONG:
            e.which_value = Payload_DataSet_DataSetValue_long_value_tag;
            e.value.long_value = sc.l;
            break;
        case W_FLOAT:
            e.which_value = Payload_DataSet_DataSetValue_float_value_tag;
            e.value.float_value = sc.f;
            break;
        case W_DOUBLE:
            e.which_value = Payload_DataSet_DataSetValue_double_value_tag;
            e.value.double_value = sc.d;
            break;
        case W_BOOL:
            e.which_value = Payload_DataSet_DataSetValue_boolean_value_tag;
            e.value.boolean_value = sc.b;
            break;
        case W_STRING:
            e.which_value = Payload_DataSet_DataSetValue_string_value_tag;
            if (!copy_str(e.value.string_value, sizeof(e.value.string_value), sc.s)) {
                return false;
            }
            break;
        default:
            return false;   /* a column type a DataSet cannot carry */
        }
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, Payload_DataSet_DataSetValue_fields, &e)) {
            return false;
        }
    }
    return true;
}

static bool enc_ds_rows(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    const spb_dataset_t *ds = *arg;
    for (uint32_t row = 0; row < ds->rows; row++) {
        ds_row_t r = {.ds = ds, .row = row};
        Payload_DataSet_Row pr = Payload_DataSet_Row_init_zero;
        pr.elements.funcs.encode = enc_ds_elements;
        pr.elements.arg = &r;
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, Payload_DataSet_Row_fields, &pr)) {
            return false;
        }
    }
    return true;
}

/* ---------- metrics ---------- */

static bool fill_metric(Payload_Metric *out, const spb_metric_t *m);

/* SPB-18 template members: each one is a whole metric, encoded with its own properties. */
static bool enc_tmpl_members(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    const spb_template_t *t = *arg;
    for (uint32_t i = 0; i < t->count; i++) {
        spb_metric_t mm;
        if (t->members != NULL) {
            mm = t->members[i];
        } else {
            memset(&mm, 0, sizeof(mm));
            t->get(t->ctx, i, &mm);
        }
        if (mm.datatype == SPB_DT_TEMPLATE) {
            return false;   /* one level only */
        }
        Payload_Metric pm;
        if (!fill_metric(&pm, &mm) || !pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, Payload_Metric_fields, &pm)) {
            return false;
        }
    }
    return true;
}

/* SPB-18 template parameters (#251). tck-id-payloads-template-parameter-name-required and
 * -type-req: each one has its name and its type. tck-id-payloads-template-parameter-type-value
 * and -value: only a basic datatype, in the value field of that datatype. */
static bool enc_tmpl_params(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    const spb_template_t *t = *arg;
    for (uint32_t i = 0; i < t->param_count; i++) {
        const spb_param_t *p = &t->params[i];
        Payload_Template_Parameter pp = Payload_Template_Parameter_init_zero;
        pp.has_name = true;
        if (!copy_str(pp.name, sizeof(pp.name), p->name)) {
            return false;
        }
        pp.has_type = true;
        pp.type = p->datatype;
        scalar_t sc = scalar(p->datatype, &p->v);
        switch (sc.wire) {
        case W_INT:
            pp.which_value = Payload_Template_Parameter_int_value_tag;
            pp.value.int_value = sc.i;
            break;
        case W_LONG:
            pp.which_value = Payload_Template_Parameter_long_value_tag;
            pp.value.long_value = sc.l;
            break;
        case W_FLOAT:
            pp.which_value = Payload_Template_Parameter_float_value_tag;
            pp.value.float_value = sc.f;
            break;
        case W_DOUBLE:
            pp.which_value = Payload_Template_Parameter_double_value_tag;
            pp.value.double_value = sc.d;
            break;
        case W_BOOL:
            pp.which_value = Payload_Template_Parameter_boolean_value_tag;
            pp.value.boolean_value = sc.b;
            break;
        case W_STRING:
            pp.which_value = Payload_Template_Parameter_string_value_tag;
            if (!copy_str(pp.value.string_value, sizeof(pp.value.string_value), sc.s)) {
                return false;
            }
            break;
        default:
            return false;   /* a datatype a parameter cannot carry */
        }
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, Payload_Template_Parameter_fields, &pp)) {
            return false;
        }
    }
    return true;
}

static bool fill_metric(Payload_Metric *out, const spb_metric_t *m)
{
    *out = (Payload_Metric)Payload_Metric_init_zero;
    out->properties.funcs.encode = enc_props;
    out->properties.arg = (void *)&m->props;
    if (m->name != NULL) {
        out->has_name = true;
        if (!copy_str(out->name, sizeof(out->name), m->name)) {
            return false;
        }
    }
    out->has_alias = !m->no_alias;
    out->alias = m->no_alias ? 0 : m->alias;
    /* tck-id-payloads-metric-datatype-req: births carry it; tck-id-payloads-metric-datatype-not-req:
     * DATA does not (omit_datatype, #159) */
    out->has_datatype = !m->omit_datatype;
    out->datatype = m->omit_datatype ? 0 : m->datatype;
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
        return true;
    }
    switch (m->datatype) {
    case SPB_DT_BYTES:
    case SPB_DT_FILE:
        if (m->v.bytes.len > sizeof(out->value.bytes_value.bytes)) {
            return false;
        }
        out->which_value = Payload_Metric_bytes_value_tag;
        out->value.bytes_value.size = (pb_size_t)m->v.bytes.len;
        if (m->v.bytes.len > 0) {
            memcpy(out->value.bytes_value.bytes, m->v.bytes.data, m->v.bytes.len);
        }
        return true;
    case SPB_DT_DATASET: {
        const spb_dataset_t *ds = m->v.ds;
        out->which_value = Payload_Metric_dataset_value_tag;
        Payload_DataSet *pd = &out->value.dataset_value;
        *pd = (Payload_DataSet)Payload_DataSet_init_zero;
        pd->has_num_of_columns = true;
        pd->num_of_columns = ds->columns;
        pd->columns.funcs.encode = enc_ds_columns;
        pd->columns.arg = (void *)ds;
        pd->types.funcs.encode = enc_ds_types;
        pd->types.arg = (void *)ds;
        pd->rows.funcs.encode = enc_ds_rows;
        pd->rows.arg = (void *)ds;
        return true;
    }
    case SPB_DT_TEMPLATE: {
        const spb_template_t *t = m->v.tmpl;
        out->which_value = Payload_Metric_template_value_tag;
        Payload_Template *pt = &out->value.template_value;
        *pt = (Payload_Template)Payload_Template_init_zero;
        pt->metrics.funcs.encode = enc_tmpl_members;
        pt->metrics.arg = (void *)t;
        if (t->param_count > 0) {
            pt->parameters.funcs.encode = enc_tmpl_params;
            pt->parameters.arg = (void *)t;
        }
        if (t->ref != NULL) {
            pt->has_template_ref = true;
            if (!copy_str(pt->template_ref, sizeof(pt->template_ref), t->ref)) {
                return false;
            }
        }
        pt->has_is_definition = true;
        pt->is_definition = t->is_definition;
        return true;
    }
    default:
        break;
    }
    scalar_t sc = scalar(m->datatype, &m->v);
    switch (sc.wire) {
    case W_INT:
        out->which_value = Payload_Metric_int_value_tag;
        out->value.int_value = sc.i;
        break;
    case W_LONG:
        out->which_value = Payload_Metric_long_value_tag;
        out->value.long_value = sc.l;
        break;
    case W_FLOAT:
        out->which_value = Payload_Metric_float_value_tag;
        out->value.float_value = sc.f;
        break;
    case W_DOUBLE:
        out->which_value = Payload_Metric_double_value_tag;
        out->value.double_value = sc.d;
        break;
    case W_BOOL:
        out->which_value = Payload_Metric_boolean_value_tag;
        out->value.boolean_value = sc.b;
        break;
    case W_STRING:
        out->which_value = Payload_Metric_string_value_tag;
        if (!copy_str(out->value.string_value, sizeof(out->value.string_value), sc.s)) {
            return false;
        }
        break;
    default:
        out->has_is_null = true;
        out->is_null = true;
        break;
    }
    return true;
}

/* #173: one metric of spb_payload_t.extra; its scratch stays off the frame of enc_metrics. */
static __attribute__((noinline)) bool enc_extra(pb_ostream_t *stream, const pb_field_iter_t *field,
                                                const spb_payload_t *p, size_t i)
{
    spb_metric_t mm;
    char name[SPB_NAME_MAX + 1] = "";
    memset(&mm, 0, sizeof(mm));
    p->extra(p->extra_ctx, i, &mm, name);
    Payload_Metric m;
    return fill_metric(&m, &mm) && pb_encode_tag_for_field(stream, field) &&
           pb_encode_submessage(stream, Payload_Metric_fields, &m);
}

static bool enc_metrics(pb_ostream_t *stream, const pb_field_iter_t *field, void *const *arg)
{
    const spb_payload_t *p = *arg;
    Payload_Metric m;
    for (size_t i = 0; i < p->metric_count; i++) {
        if (!fill_metric(&m, &p->metrics[i]) || !pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, Payload_Metric_fields, &m)) {
            return false;
        }
    }
    for (size_t i = 0; p->extra != NULL && i < p->extra_count; i++) {
        if (!enc_extra(stream, field, p, i)) {
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
    const spb_decoder_t *d;   /* NULL: validate only */
} dec_ctx_t;

/* Decoding of a PropertySet into an spb_rx_metric_t: keys and values arrive as two repeated
 * fields and are paired by position. */
typedef struct {
    spb_rx_metric_t *rx;
    uint8_t keys;
    uint8_t values;
} dec_props_t;

static bool dec_prop_key(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    dec_props_t *d = *arg;
    size_t len = stream->bytes_left;
    if (d->keys < SPB_RX_PROPS) {
        char *key = d->rx->props[d->keys].key;
        size_t take = len < SPB_RX_KEY_MAX ? len : SPB_RX_KEY_MAX;
        if (!pb_read(stream, (pb_byte_t *)key, take)) {
            return false;
        }
        key[take] = '\0';
        len -= take;
    }
    if (d->keys < UINT8_MAX) {
        d->keys++;
    }
    return pb_read(stream, NULL, len);   /* the rest of a long key */
}

static bool dec_prop_value(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    dec_props_t *d = *arg;
    Payload_PropertyValue pv = Payload_PropertyValue_init_zero;
    if (!pb_decode(stream, Payload_PropertyValue_fields, &pv)) {
        return false;
    }
    if (d->values < SPB_RX_PROPS) {
        spb_rx_prop_t *p = &d->rx->props[d->values];
        p->type = pv.type;
        p->is_null = pv.has_is_null && pv.is_null;
        switch (pv.which_value) {
        case Payload_PropertyValue_int_value_tag:
            p->kind = SPB_VAL_INT;
            p->v.i = pv.value.int_value;
            break;
        case Payload_PropertyValue_long_value_tag:
            p->kind = SPB_VAL_LONG;
            p->v.l = pv.value.long_value;
            break;
        case Payload_PropertyValue_float_value_tag:
            p->kind = SPB_VAL_FLOAT;
            p->v.f = pv.value.float_value;
            break;
        case Payload_PropertyValue_double_value_tag:
            p->kind = SPB_VAL_DOUBLE;
            p->v.d = pv.value.double_value;
            break;
        case Payload_PropertyValue_boolean_value_tag:
            p->kind = SPB_VAL_BOOL;
            p->v.b = pv.value.boolean_value;
            break;
        case Payload_PropertyValue_string_value_tag:
            p->kind = SPB_VAL_STRING;
            memcpy(p->s, pv.value.string_value, sizeof(p->s));
            break;
        default:
            p->kind = SPB_VAL_NONE;
            break;
        }
    }
    if (d->values < UINT8_MAX) {
        d->values++;
    }
    return true;
}

static bool dec_props(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    dec_props_t *d = *arg;
    Payload_PropertySet ps = Payload_PropertySet_init_zero;
    ps.keys.funcs.decode = dec_prop_key;
    ps.keys.arg = d;
    ps.values.funcs.decode = dec_prop_value;
    ps.values.arg = d;
    if (!pb_decode(stream, Payload_PropertySet_fields, &ps)) {
        return false;
    }
    d->rx->prop_count = d->keys < d->values ? d->keys : d->values;
    return true;
}

/* ---------- SPB-19 DataSet decoding: names and cells are streamed to the decoder ---------- */

typedef struct {
    const spb_decoder_t *d;
    spb_rx_metric_t *rx;
    uint32_t col;             /* next element of the current row */
} dec_value_t;

static bool decode_metric(pb_istream_t *stream, const spb_decoder_t *d, bool member);

static bool dec_ds_column(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    dec_value_t *dv = *arg;
    char name[SPB_NAME_MAX + 1];
    size_t len = stream->bytes_left;
    size_t take = len < SPB_NAME_MAX ? len : SPB_NAME_MAX;
    if (!pb_read(stream, (pb_byte_t *)name, take) || !pb_read(stream, NULL, len - take)) {
        return false;
    }
    name[take] = '\0';
    uint32_t col = dv->rx->ds.columns;
    if (dv->rx->ds.columns < UINT32_MAX) {
        dv->rx->ds.columns++;
    }
    return dv->d == NULL || dv->d->column == NULL || dv->d->column(dv->d->ctx, col, name);
}

static bool dec_ds_type(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    dec_value_t *dv = *arg;
    uint32_t t;
    if (!pb_decode_varint32(stream, &t)) {
        return false;
    }
    if (dv->rx->ds.type_count < sizeof(dv->rx->ds.types) / sizeof(dv->rx->ds.types[0])) {
        dv->rx->ds.types[dv->rx->ds.type_count++] = t;
    }
    return true;
}

static bool dec_ds_element(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    dec_value_t *dv = *arg;
    Payload_DataSet_DataSetValue e = Payload_DataSet_DataSetValue_init_zero;
    if (!pb_decode(stream, Payload_DataSet_DataSetValue_fields, &e)) {
        return false;
    }
    spb_rx_value_t v = {.kind = SPB_VAL_NONE};
    switch (e.which_value) {
    case Payload_DataSet_DataSetValue_int_value_tag:
        v.kind = SPB_VAL_INT;
        v.v.i = e.value.int_value;
        break;
    case Payload_DataSet_DataSetValue_long_value_tag:
        v.kind = SPB_VAL_LONG;
        v.v.l = e.value.long_value;
        break;
    case Payload_DataSet_DataSetValue_float_value_tag:
        v.kind = SPB_VAL_FLOAT;
        v.v.f = e.value.float_value;
        break;
    case Payload_DataSet_DataSetValue_double_value_tag:
        v.kind = SPB_VAL_DOUBLE;
        v.v.d = e.value.double_value;
        break;
    case Payload_DataSet_DataSetValue_boolean_value_tag:
        v.kind = SPB_VAL_BOOL;
        v.v.b = e.value.boolean_value;
        break;
    case Payload_DataSet_DataSetValue_string_value_tag:
        v.kind = SPB_VAL_STRING;
        v.v.s = e.value.string_value;
        break;
    default:
        break;
    }
    uint32_t col = dv->col++;
    return dv->d == NULL || dv->d->cell == NULL ||
           dv->d->cell(dv->d->ctx, dv->rx->ds.rows, col, &v);
}

static bool dec_ds_row(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    dec_value_t *dv = *arg;
    Payload_DataSet_Row r = Payload_DataSet_Row_init_zero;
    r.elements.funcs.decode = dec_ds_element;
    r.elements.arg = dv;
    dv->col = 0;
    if (!pb_decode(stream, Payload_DataSet_Row_fields, &r)) {
        return false;
    }
    if (dv->rx->ds.rows < UINT32_MAX) {
        dv->rx->ds.rows++;
    }
    return true;
}

/* SPB-18: a template member, handed to the decoder's member callback. */
static bool dec_tmpl_member(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    dec_value_t *dv = *arg;
    if (dv->rx->tmpl.members < UINT32_MAX) {
        dv->rx->tmpl.members++;
    }
    return decode_metric(stream, dv->d, true);
}

/* SPB-18 (#251): a template parameter, handed to the decoder's param callback. */
static bool dec_tmpl_param(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    dec_value_t *dv = *arg;
    Payload_Template_Parameter pp = Payload_Template_Parameter_init_zero;
    if (!pb_decode(stream, Payload_Template_Parameter_fields, &pp)) {
        return false;
    }
    spb_rx_param_t p = {.name = pp.has_name ? pp.name : NULL, .has_type = pp.has_type,
                        .type = pp.type, .value = {.kind = SPB_VAL_NONE}};
    switch (pp.which_value) {
    case Payload_Template_Parameter_int_value_tag:
        p.value.kind = SPB_VAL_INT;
        p.value.v.i = pp.value.int_value;
        break;
    case Payload_Template_Parameter_long_value_tag:
        p.value.kind = SPB_VAL_LONG;
        p.value.v.l = pp.value.long_value;
        break;
    case Payload_Template_Parameter_float_value_tag:
        p.value.kind = SPB_VAL_FLOAT;
        p.value.v.f = pp.value.float_value;
        break;
    case Payload_Template_Parameter_double_value_tag:
        p.value.kind = SPB_VAL_DOUBLE;
        p.value.v.d = pp.value.double_value;
        break;
    case Payload_Template_Parameter_boolean_value_tag:
        p.value.kind = SPB_VAL_BOOL;
        p.value.v.b = pp.value.boolean_value;
        break;
    case Payload_Template_Parameter_string_value_tag:
        p.value.kind = SPB_VAL_STRING;
        p.value.v.s = pp.value.string_value;
        break;
    default:
        break;
    }
    if (dv->rx->tmpl.params < UINT32_MAX) {
        dv->rx->tmpl.params++;
    }
    return dv->d->param(dv->d->ctx, &p);
}

/* nanopb calls this before it decodes a submessage of the value oneof: its callbacks are set
 * here (the union member was just cleared). */
static bool dec_value_submsg(pb_istream_t *stream, const pb_field_t *field, void **arg)
{
    (void)stream;
    dec_value_t *dv = *arg;
    if (field->tag == Payload_Metric_template_value_tag) {
        Payload_Template *t = field->pData;
        t->metrics.funcs.decode = dec_tmpl_member;
        t->metrics.arg = dv;
        /* #251: only for a decoder that asks for the parameters. Without the callback nanopb
         * skips the field, so a command with parameters that this codec cannot hold (a long
         * name or String) is still decoded: the engine ignores the parameters of a command. */
        if (dv->d != NULL && dv->d->param != NULL) {
            t->parameters.funcs.decode = dec_tmpl_param;
            t->parameters.arg = dv;
        }
        return true;
    }
    if (field->tag == Payload_Metric_dataset_value_tag) {
        Payload_DataSet *ds = field->pData;
        ds->columns.funcs.decode = dec_ds_column;
        ds->columns.arg = dv;
        ds->types.funcs.decode = dec_ds_type;
        ds->types.arg = dv;
        ds->rows.funcs.decode = dec_ds_row;
        ds->rows.arg = dv;
    }
    return true;
}

/* Decode one metric and hand it to d->metric, or to d->member for a template member (@p member).
 * A member gets no DataSet or Template callbacks: one level only. */
static bool decode_metric(pb_istream_t *stream, const spb_decoder_t *d, bool member)
{
    spb_rx_metric_t rx;
    memset(&rx, 0, sizeof(rx));
    dec_props_t dp = {.rx = &rx};
    dec_value_t dv = {.d = d, .rx = &rx};
    Payload_Metric m = Payload_Metric_init_zero;
    m.properties.funcs.decode = dec_props;
    m.properties.arg = &dp;
    if (!member) {
        m.cb_value.funcs.decode = dec_value_submsg;
        m.cb_value.arg = &dv;
    }
    if (!pb_decode(stream, Payload_Metric_fields, &m)) {
        return false;
    }
    rx.name = m.has_name ? m.name : NULL;
    rx.has_alias = m.has_alias;
    rx.alias = m.alias;
    rx.has_datatype = m.has_datatype;
    rx.datatype = m.datatype;
    rx.timestamp = m.has_timestamp ? m.timestamp : 0;
    rx.is_null = m.has_is_null && m.is_null;
    rx.is_historical = m.has_is_historical && m.is_historical;
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
    case Payload_Metric_bytes_value_tag:
        rx.kind = SPB_VAL_BYTES;
        rx.v.bytes.data = m.value.bytes_value.bytes;
        rx.v.bytes.len = m.value.bytes_value.size;
        break;
    case Payload_Metric_dataset_value_tag:
        rx.kind = SPB_VAL_DATASET;
        rx.ds.num_of_columns = m.value.dataset_value.num_of_columns;
        break;
    case Payload_Metric_template_value_tag:
        rx.kind = SPB_VAL_TEMPLATE;
        rx.tmpl.has_ref = m.value.template_value.has_template_ref;
        memcpy(rx.tmpl.ref, m.value.template_value.template_ref, sizeof(rx.tmpl.ref));
        rx.tmpl.is_definition = m.value.template_value.has_is_definition &&
                                m.value.template_value.is_definition;
        break;
    default:
        rx.kind = SPB_VAL_NONE;
        break;
    }
    spb_metric_cb_t cb = d == NULL ? NULL : member ? d->member : d->metric;
    return cb == NULL || cb(d->ctx, &rx);
}

static bool dec_metric(pb_istream_t *stream, const pb_field_iter_t *field, void **arg)
{
    (void)field;
    const dec_ctx_t *dc = *arg;
    return decode_metric(stream, dc->d, false);
}

bool spb_decode(const uint8_t *buf, size_t len, spb_rx_header_t *hdr, spb_metric_cb_t cb,
                void *ctx)
{
    const spb_decoder_t d = {.metric = cb, .ctx = ctx};
    return spb_decode_with(buf, len, hdr, cb != NULL ? &d : NULL);
}

bool spb_decode_with(const uint8_t *buf, size_t len, spb_rx_header_t *hdr, const spb_decoder_t *d)
{
    dec_ctx_t dc = {.d = d};
    Payload msg = Payload_init_zero;
    msg.metrics.funcs.decode = dec_metric;
    msg.metrics.arg = &dc;

    pb_istream_t is = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&is, Payload_fields, &msg)) {
        s_last_error = PB_GET_ERROR(&is);
        return false;
    }
    s_last_error = "";
    if (hdr != NULL) {
        hdr->has_timestamp = msg.has_timestamp;
        hdr->timestamp = msg.timestamp;
        hdr->has_seq = msg.has_seq;
        hdr->seq = msg.seq;
    }
    return true;
}

const spb_rx_prop_t *spb_rx_prop(const spb_rx_metric_t *m, const char *key)
{
    size_t n = m->prop_count < SPB_RX_PROPS ? m->prop_count : SPB_RX_PROPS;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(m->props[i].key, key) == 0) {
            return &m->props[i];
        }
    }
    return NULL;
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
