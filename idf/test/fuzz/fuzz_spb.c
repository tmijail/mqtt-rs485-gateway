/*
 * Issue #226: MQTT. A Sparkplug B payload from the broker (NCMD, DCMD, or any message of the
 * namespace): spb_decode_with() with every callback, and what the callbacks of gw_node read
 * from a metric (spb_rx_number for each datatype, spb_rx_prop, the strings, the bytes, the
 * DataSet columns and cells and the Template members).
 *
 * Input: a control byte (bits 0-3: stop after that many callbacks, 0 = never; bit 4: validate
 * only, as the first pass of an NCMD), then the payload.
 */
#include "fuzz.h"
#include "spb.h"

typedef struct {
    unsigned calls;
    unsigned stop_at;
    volatile size_t sink;
} ctx_t;

static bool more(ctx_t *c)
{
    c->calls++;
    return c->stop_at == 0 || c->calls < c->stop_at;
}

static void read_metric(ctx_t *c, const spb_rx_metric_t *m)
{
    if (m->name != NULL) {
        c->sink += strlen(m->name);
    }
    switch (m->kind) {
    case SPB_VAL_STRING:
        FZ_CHECK(m->v.s != NULL);
        c->sink += strlen(m->v.s);
        break;
    case SPB_VAL_BYTES:
        for (size_t i = 0; i < m->v.bytes.len; i++) {
            c->sink += m->v.bytes.data[i];
        }
        break;
    case SPB_VAL_TEMPLATE:
        FZ_CHECK(memchr(m->tmpl.ref, 0, sizeof(m->tmpl.ref)) != NULL);
        break;
    default:
        break;
    }
    FZ_CHECK(m->ds.type_count <= 16);
    for (size_t i = 0; i < SPB_RX_PROPS && i < m->prop_count; i++) {
        const spb_rx_prop_t *p = &m->props[i];
        FZ_CHECK(memchr(p->key, 0, sizeof(p->key)) != NULL);
        FZ_CHECK(memchr(p->s, 0, sizeof(p->s)) != NULL);
        FZ_CHECK(spb_rx_prop(m, p->key) != NULL);
    }
    c->sink += spb_rx_prop(m, "engUnit") != NULL;
    c->sink += spb_rx_prop(m, "") != NULL;
    double d;
    if (spb_rx_number(m, m->datatype, &d)) {
        c->sink += (size_t)(d != d);
    }
    for (uint32_t t = 0; t <= 34; t++) {
        if (spb_rx_number(m, t, &d)) {
            c->sink += (size_t)(d != d);
        }
    }
}

static bool on_metric(void *ctx, const spb_rx_metric_t *m)
{
    ctx_t *c = ctx;
    read_metric(c, m);
    return more(c);
}

static bool on_member(void *ctx, const spb_rx_metric_t *m)
{
    ctx_t *c = ctx;
    read_metric(c, m);
    return more(c);
}

static bool on_column(void *ctx, uint32_t col, const char *name)
{
    ctx_t *c = ctx;
    FZ_CHECK(name != NULL);
    c->sink += strlen(name) + col;
    return more(c);
}

static bool on_cell(void *ctx, uint32_t row, uint32_t col, const spb_rx_value_t *v)
{
    ctx_t *c = ctx;
    c->sink += row + col;
    if (v->kind == SPB_VAL_STRING) {
        FZ_CHECK(v->v.s != NULL);
        c->sink += strlen(v->v.s);
    }
    return more(c);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t ctl = fz_u8(&f);
    size_t len;
    const uint8_t *rest = fz_rest(&f, &len);
    uint8_t *buf = fz_dup(rest, len);

    ctx_t c = {.stop_at = ctl & 15u};
    spb_decoder_t d = {on_metric, on_column, on_cell, on_member, &c};
    spb_rx_header_t hdr;
    bool ok = spb_decode_with(buf, len, &hdr, (ctl & 16u) != 0 ? NULL : &d);
    const char *why = spb_last_error();
    FZ_CHECK(why != NULL);
    FZ_CHECK(!ok || why[0] == '\0');
    /* the two passes of a command: a payload that the validation accepts decodes in full */
    if (ok && (ctl & 16u) != 0) {
        c.stop_at = 0;
        FZ_CHECK(spb_decode_with(buf, len, &hdr, &d));
    }
    free(buf);
    return 0;
}
