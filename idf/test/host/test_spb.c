/* QA-1: Sparkplug B payload encoding/decoding (SPB-1). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spb.h"
#include "support.h"
#include "unity.h"

static uint8_t buf[GW_SPB_PAYLOAD_MAX];
static decoded_t dec;

void setUp(void) {}
void tearDown(void) {}

static void test_round_trip_all_types(void)
{
    spb_metric_t m[] = {
        {.name = "b", .alias = 1, .datatype = SPB_DT_BOOLEAN, .timestamp = 5, .v.num = 1},
        {.name = "i16", .alias = 2, .datatype = SPB_DT_INT16, .v.num = -5},
        {.name = "u16", .alias = 3, .datatype = SPB_DT_UINT16, .v.num = 65535},
        {.name = "i32", .alias = 4, .datatype = SPB_DT_INT32, .v.num = -2147483648.0},
        {.name = "u32", .alias = 5, .datatype = SPB_DT_UINT32, .v.num = 4294967295.0},
        {.name = "f", .alias = 6, .datatype = SPB_DT_FLOAT, .v.num = 230.5},
        {.name = "u64", .alias = 7, .datatype = SPB_DT_UINT64, .v.u64 = 255},
        {.name = "s", .alias = 8, .datatype = SPB_DT_STRING, .v.str = "1.2.3+gabc"},
        {.name = NULL, .alias = 9, .datatype = SPB_DT_FLOAT, .is_null = true},
    };
    spb_payload_t p = {.timestamp = 1700000000123ull, .has_seq = true, .seq = 42,
                       .metrics = m, .metric_count = 9};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_TRUE(dec.has_seq);
    TEST_ASSERT_EQUAL_UINT64(42, dec.seq);
    TEST_ASSERT_EQUAL_UINT64(1700000000123ull, dec.timestamp);
    TEST_ASSERT_EQUAL(9, dec.count);
    TEST_ASSERT_EQUAL_DOUBLE(1, dec.m[0].num);
    TEST_ASSERT_EQUAL_UINT64(5, dec.m[0].timestamp);
    TEST_ASSERT_EQUAL(SPB_VAL_BOOL, dec.m[0].kind);
    TEST_ASSERT_EQUAL_DOUBLE(-5, dec.m[1].num);
    TEST_ASSERT_EQUAL(SPB_VAL_INT, dec.m[1].kind);
    TEST_ASSERT_EQUAL_DOUBLE(65535, dec.m[2].num);
    TEST_ASSERT_EQUAL_DOUBLE(-2147483648.0, dec.m[3].num);
    TEST_ASSERT_EQUAL_DOUBLE(4294967295.0, dec.m[4].num);
    TEST_ASSERT_EQUAL_DOUBLE(230.5, dec.m[5].num);
    TEST_ASSERT_EQUAL(SPB_VAL_FLOAT, dec.m[5].kind);
    TEST_ASSERT_EQUAL(SPB_VAL_LONG, dec.m[6].kind);
    TEST_ASSERT_EQUAL_STRING("1.2.3+gabc", dec.m[7].str);
    TEST_ASSERT_FALSE(dec.m[8].has_name);
    TEST_ASSERT_TRUE(dec.m[8].is_null);
    TEST_ASSERT_EQUAL(SPB_VAL_NONE, dec.m[8].kind);
    TEST_ASSERT_EQUAL_UINT64(9, dec.m[8].alias);
}

/* MB-6: 64-bit integers travel exactly in long_value (two's complement for Int64), Double in
 * double_value. */
static void test_round_trip_64bit_exact(void)
{
    spb_metric_t m[] = {
        {.alias = 1, .datatype = SPB_DT_INT64, .v.i64 = -9007199254740993ll},
        {.alias = 2, .datatype = SPB_DT_UINT64, .v.u64 = UINT64_MAX},
        {.alias = 3, .datatype = SPB_DT_DOUBLE, .v.num = 1234.56789012345},
        {.alias = 4, .datatype = SPB_DT_INT64, .v.i64 = INT64_MIN},
    };
    spb_payload_t p = {.timestamp = 1, .has_seq = true, .seq = 0, .metrics = m, .metric_count = 4};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_EQUAL(SPB_VAL_LONG, dec.m[0].kind);
    TEST_ASSERT_EQUAL_INT64(-9007199254740993ll, (int64_t)dec.m[0].u64);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, dec.m[1].u64);
    TEST_ASSERT_EQUAL(SPB_VAL_DOUBLE, dec.m[2].kind);
    TEST_ASSERT_EQUAL_DOUBLE(1234.56789012345, dec.m[2].num);
    TEST_ASSERT_EQUAL_UINT64(0x8000000000000000ull, dec.m[3].u64);
}

static void test_wire_format_int16_sign_extension(void)
{
    /* Sparkplug B: Int16 -5 is carried in int_value as the 32-bit two's complement 0xFFFFFFFB. */
    spb_metric_t m = {.alias = 1, .datatype = SPB_DT_INT16, .v.num = -5};
    spb_payload_t p = {.timestamp = 1, .has_seq = true, .seq = 0, .metrics = &m, .metric_count = 1};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    /* metric submessage: alias(2)=1, datatype(4)=2, int_value(10)=0xFFFFFFFB as varint */
    const uint8_t expect[] = {0x10, 0x01, 0x20, 0x02, 0x50, 0xFB, 0xFF, 0xFF, 0xFF, 0x0F};
    TEST_ASSERT_GREATER_OR_EQUAL(sizeof(expect), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf + len - sizeof(expect) - 2, sizeof(expect));
}

static void test_ndeath_has_no_seq(void)
{
    size_t len = gw_node_death_payload(7, buf, sizeof(buf));
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_FALSE(dec.has_seq);
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL_STRING("bdSeq", dec.m[0].name);
    TEST_ASSERT_EQUAL(SPB_DT_UINT64, dec.m[0].datatype);
    TEST_ASSERT_EQUAL_DOUBLE(7, dec.m[0].num);
}

static void test_rx_number_variants(void)
{
    spb_rx_metric_t m = {.kind = SPB_VAL_INT, .v.i = 0xFFFB};  /* not sign-extended by sender */
    double v;
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_INT16, &v));
    TEST_ASSERT_EQUAL_DOUBLE(-5, v);
    m.v.i = 0xFFFFFFFB;
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_INT16, &v));
    TEST_ASSERT_EQUAL_DOUBLE(-5, v);
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_UINT32, &v));
    TEST_ASSERT_EQUAL_DOUBLE(4294967291.0, v);
    m.kind = SPB_VAL_FLOAT;
    m.v.f = 12.3f;
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_FLOAT, &v));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 12.3, v);
    m.kind = SPB_VAL_BOOL;
    m.v.b = true;
    TEST_ASSERT_TRUE(spb_rx_number(&m, SPB_DT_BOOLEAN, &v));
    TEST_ASSERT_EQUAL_DOUBLE(1, v);
    m.kind = SPB_VAL_STRING;
    TEST_ASSERT_FALSE(spb_rx_number(&m, SPB_DT_STRING, &v));
    m.kind = SPB_VAL_DOUBLE;
    m.is_null = true;
    TEST_ASSERT_FALSE(spb_rx_number(&m, SPB_DT_DOUBLE, &v));
}

static void test_decode_rejects_garbage(void)
{
    const uint8_t junk[] = {0x12, 0xFF, 0x01, 0x02};  /* metrics field with impossible length */
    TEST_ASSERT_FALSE(decode_payload(junk, sizeof(junk), &dec));
    /* unknown/ignored fields (uuid, bytes_value) are skipped */
    const uint8_t with_uuid[] = {0x08, 0x01, 0x22, 0x03, 'a', 'b', 'c', 0x18, 0x05};
    TEST_ASSERT_TRUE(decode_payload(with_uuid, sizeof(with_uuid), &dec));
    TEST_ASSERT_EQUAL_UINT64(5, dec.seq);
}

/* SPB-16: engUnit, engLow, engHigh and Quality travel in the metric's PropertySet, keys and
 * values paired by position; a metric without properties has no PropertySet at all. */
static void test_properties_round_trip(void)
{
    spb_metric_t m[] = {
        {.name = "Voltage", .alias = 100, .datatype = SPB_DT_FLOAT, .v.num = 230.5,
         .props = {.eng_unit = "V", .has_eng_low = true, .eng_low = -10.5, .has_eng_high = true,
                   .eng_high = 400, .has_quality = true, .quality = SPB_QUALITY_GOOD}},
        {.alias = 101, .datatype = SPB_DT_FLOAT, .is_null = true,
         .props = {.has_quality = true, .quality = SPB_QUALITY_BAD}},
        {.alias = 102, .datatype = SPB_DT_FLOAT, .v.num = 1, .props = {.eng_unit = ""}},
    };
    spb_payload_t p = {.timestamp = 1, .has_seq = true, .seq = 0, .metrics = m, .metric_count = 3};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_EQUAL(3, dec.count);
    TEST_ASSERT_EQUAL(4, dec.m[0].prop_count);
    TEST_ASSERT_EQUAL_STRING("V", dec.m[0].eng_unit);
    TEST_ASSERT_TRUE(dec.m[0].has_eng_low);
    TEST_ASSERT_EQUAL_DOUBLE(-10.5, dec.m[0].eng_low);
    TEST_ASSERT_TRUE(dec.m[0].has_eng_high);
    TEST_ASSERT_EQUAL_DOUBLE(400, dec.m[0].eng_high);
    TEST_ASSERT_TRUE(dec.m[0].has_quality);
    TEST_ASSERT_EQUAL_INT32(192, dec.m[0].quality);
    TEST_ASSERT_EQUAL_DOUBLE(230.5, dec.m[0].num);   /* the value is still there */
    TEST_ASSERT_TRUE(dec.m[1].is_null);
    TEST_ASSERT_EQUAL(1, dec.m[1].prop_count);
    TEST_ASSERT_TRUE(dec.m[1].has_quality);
    TEST_ASSERT_EQUAL_INT32(0, dec.m[1].quality);
    TEST_ASSERT_EQUAL(0, dec.m[2].prop_count);        /* "" engUnit is omitted */

    /* wire format: properties(9) { keys(1) "Quality", values(2) { type(1)=3, int_value(3)=0 } } */
    spb_metric_t q = {.alias = 1, .datatype = SPB_DT_BOOLEAN, .is_null = true,
                      .props = {.has_quality = true, .quality = SPB_QUALITY_BAD}};
    p.metrics = &q;
    p.metric_count = 1;
    len = spb_encode(&p, buf, sizeof(buf));
    const uint8_t expect[] = {0x4A, 0x0F, 0x0A, 0x07, 'Q', 'u', 'a', 'l', 'i', 't', 'y',
                              0x12, 0x04, 0x08, 0x03, 0x18, 0x00};
    TEST_ASSERT_GREATER_OR_EQUAL(sizeof(expect), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, buf + len - sizeof(expect) - 2, sizeof(expect));
}

/* A long property key is truncated, not rejected; properties past SPB_RX_PROPS are counted. */
static void test_property_decode_bounds(void)
{
    /* Payload { metrics { alias 1, properties { keys x5 (first one 40 chars), values x5 } } } */
    uint8_t raw[256];
    size_t n = 0;
    uint8_t ps[220];
    size_t k = 0;
    for (int i = 0; i < 5; i++) {
        size_t klen = i == 0 ? 40 : 1;
        ps[k++] = 0x0A;
        ps[k++] = (uint8_t)klen;
        memset(ps + k, 'a' + i, klen);
        k += klen;
    }
    for (int i = 0; i < 5; i++) {
        const uint8_t pv[] = {0x12, 0x04, 0x08, 0x07, 0x18, (uint8_t)(10 + i)};
        memcpy(ps + k, pv, sizeof(pv));
        k += sizeof(pv);
    }
    uint8_t metric[240];
    size_t mlen = 0;
    metric[mlen++] = 0x10;   /* alias */
    metric[mlen++] = 0x01;
    metric[mlen++] = 0x4A;   /* properties */
    metric[mlen++] = (uint8_t)k;
    memcpy(metric + mlen, ps, k);
    mlen += k;
    raw[n++] = 0x12;
    raw[n++] = (uint8_t)(0x80 | (mlen & 0x7F));
    raw[n++] = (uint8_t)(mlen >> 7);
    memcpy(raw + n, metric, mlen);
    n += mlen;
    TEST_ASSERT_TRUE(decode_payload(raw, n, &dec));
    TEST_ASSERT_EQUAL(1, dec.count);
    TEST_ASSERT_EQUAL(5, dec.m[0].prop_count);
}

/* SPB-19: Bytes and File travel in bytes_value (NUL and any byte included), up to 64 bytes. */
static void test_bytes_and_file(void)
{
    static const uint8_t raw[5] = {0x00, 0xFF, 0x10, 0x00, 0x7F};
    static uint8_t big[SPB_BYTES_MAX + 1];
    spb_metric_t m[] = {
        {.alias = 1, .datatype = SPB_DT_BYTES, .v.bytes = {raw, sizeof(raw)}},
        {.alias = 2, .datatype = SPB_DT_FILE, .v.bytes = {big, SPB_BYTES_MAX}},
        {.alias = 3, .datatype = SPB_DT_BYTES, .v.bytes = {NULL, 0}},
    };
    spb_payload_t p = {.timestamp = 1, .has_seq = true, .seq = 0, .metrics = m, .metric_count = 3};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_EQUAL(SPB_VAL_BYTES, dec.m[0].kind);
    TEST_ASSERT_EQUAL(sizeof(raw), dec.m[0].bytes_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(raw, dec.m[0].bytes, sizeof(raw));
    TEST_ASSERT_EQUAL(SPB_DT_FILE, dec.m[1].datatype);
    TEST_ASSERT_EQUAL(SPB_BYTES_MAX, dec.m[1].bytes_len);
    TEST_ASSERT_EQUAL(SPB_VAL_BYTES, dec.m[2].kind);
    TEST_ASSERT_EQUAL(0, dec.m[2].bytes_len);
    m[1].v.bytes.len = SPB_BYTES_MAX + 1;   /* too long: the payload is refused, not cut */
    TEST_ASSERT_EQUAL(0, spb_encode(&p, buf, sizeof(buf)));
}

/* SPB-19 DataSet: names, types and rows produced by a cell callback; decoded with the column
 * and cell callbacks of spb_decode_with. */
static const char *const k_cols[] = {"Device", "Online", "OK", "Total", "Ratio", "Load"};
static const uint32_t k_types[] = {SPB_DT_STRING, SPB_DT_BOOLEAN, SPB_DT_UINT32, SPB_DT_UINT64,
                                   SPB_DT_DOUBLE, SPB_DT_FLOAT};

static void ds_cell(const void *ctx, uint32_t row, uint32_t col, spb_value_t *out)
{
    (void)ctx;
    static const char *const names[] = {"Meter1", "Pump2"};
    switch (col) {
    case 0: out->str = names[row]; break;
    case 1: out->num = row == 0; break;
    case 2: out->num = 4000000000.0 + row; break;
    case 3: out->u64 = 9007199254740993ull + row; break;
    case 4: out->num = 0.25 * (row + 1); break;
    default: out->num = -1.5; break;
    }
}

typedef struct {
    char names[8][16];
    uint32_t cols;
    spb_rx_value_t cells[2][6];
    char strs[2][16];
    uint32_t cell_count;
    spb_rx_metric_t m;
} ds_rx_t;

static bool on_column(void *ctx, uint32_t col, const char *name)
{
    ds_rx_t *r = ctx;
    snprintf(r->names[col], sizeof(r->names[col]), "%s", name);
    r->cols++;
    return true;
}

static bool on_cell(void *ctx, uint32_t row, uint32_t col, const spb_rx_value_t *v)
{
    ds_rx_t *r = ctx;
    r->cells[row][col] = *v;
    if (v->kind == SPB_VAL_STRING) {
        snprintf(r->strs[row], sizeof(r->strs[row]), "%s", v->v.s);
    }
    r->cell_count++;
    return true;
}

static bool on_ds_metric(void *ctx, const spb_rx_metric_t *m)
{
    ds_rx_t *r = ctx;
    r->m = *m;
    return true;
}

static void test_dataset_round_trip(void)
{
    const spb_dataset_t ds = {.columns = 6, .names = k_cols, .types = k_types, .rows = 2,
                              .cell = ds_cell};
    spb_metric_t m = {.name = "Node Info/Modbus", .alias = 9, .datatype = SPB_DT_DATASET,
                      .v.ds = &ds};
    spb_payload_t p = {.timestamp = 1, .has_seq = true, .seq = 3, .metrics = &m, .metric_count = 1};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    static ds_rx_t r;
    memset(&r, 0, sizeof(r));
    const spb_decoder_t d = {.metric = on_ds_metric, .column = on_column, .cell = on_cell, .ctx = &r};
    TEST_ASSERT_TRUE(spb_decode_with(buf, len, NULL, &d));
    TEST_ASSERT_EQUAL(SPB_VAL_DATASET, r.m.kind);
    TEST_ASSERT_EQUAL_UINT64(6, r.m.ds.num_of_columns);
    TEST_ASSERT_EQUAL(6, r.m.ds.columns);
    TEST_ASSERT_EQUAL(2, r.m.ds.rows);
    TEST_ASSERT_EQUAL(6, r.m.ds.type_count);
    TEST_ASSERT_EQUAL_UINT32_ARRAY(k_types, r.m.ds.types, 6);
    TEST_ASSERT_EQUAL_STRING("Device", r.names[0]);
    TEST_ASSERT_EQUAL_STRING("Load", r.names[5]);
    TEST_ASSERT_EQUAL(12, r.cell_count);
    TEST_ASSERT_EQUAL_STRING("Pump2", r.strs[1]);
    TEST_ASSERT_EQUAL(SPB_VAL_BOOL, r.cells[0][1].kind);
    TEST_ASSERT_TRUE(r.cells[0][1].v.b);
    TEST_ASSERT_FALSE(r.cells[1][1].v.b);
    TEST_ASSERT_EQUAL(SPB_VAL_INT, r.cells[1][2].kind);
    TEST_ASSERT_EQUAL_UINT32(4000000001u, r.cells[1][2].v.i);
    TEST_ASSERT_EQUAL(SPB_VAL_LONG, r.cells[0][3].kind);
    TEST_ASSERT_EQUAL_UINT64(9007199254740993ull, r.cells[0][3].v.l);
    TEST_ASSERT_EQUAL_DOUBLE(0.5, r.cells[1][4].v.d);
    TEST_ASSERT_EQUAL(SPB_VAL_FLOAT, r.cells[0][5].kind);
    TEST_ASSERT_EQUAL_FLOAT(-1.5f, r.cells[0][5].v.f);
    /* the plain decoder skips the cells but still validates them */
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_EQUAL(SPB_VAL_DATASET, dec.m[0].kind);

    /* a column type a DataSet cannot carry fails the encoding */
    static const uint32_t bad[] = {SPB_DT_STRING, SPB_DT_BOOLEAN, SPB_DT_UINT32, SPB_DT_UINT64,
                                   SPB_DT_DOUBLE, SPB_DT_BYTES};
    const spb_dataset_t ds_bad = {.columns = 6, .names = k_cols, .types = bad, .rows = 1,
                                  .cell = ds_cell};
    m.v.ds = &ds_bad;
    TEST_ASSERT_EQUAL(0, spb_encode(&p, buf, sizeof(buf)));
}

/* SPB-18: a definition (no alias, is_definition, members by name) and an instance (template_ref),
 * members from an array or from a generator; members decode through the member callback. */
static void gen_member(const void *ctx, uint32_t i, spb_metric_t *out)
{
    (void)ctx;
    static const char *const names[] = {"Voltage", "Run"};
    out->name = names[i];
    out->no_alias = true;
    out->datatype = i == 0 ? SPB_DT_FLOAT : SPB_DT_BOOLEAN;
    out->props.eng_unit = i == 0 ? "V" : NULL;
}

static void test_template_round_trip(void)
{
    const spb_template_t def = {.is_definition = true, .count = 2, .get = gen_member};
    const spb_metric_t members[] = {
        {.name = "Voltage", .no_alias = true, .datatype = SPB_DT_FLOAT, .v.num = 230.5,
         .props = {.has_quality = true, .quality = SPB_QUALITY_GOOD}},
        {.name = "Run", .no_alias = true, .datatype = SPB_DT_BOOLEAN, .is_null = true},
    };
    const spb_template_t inst = {.ref = "Meter", .count = 2, .members = members};
    spb_metric_t m[] = {
        {.name = "Meter", .no_alias = true, .datatype = SPB_DT_TEMPLATE, .v.tmpl = &def},
        {.name = "Meter", .alias = 100, .datatype = SPB_DT_TEMPLATE, .v.tmpl = &inst},
    };
    spb_payload_t p = {.timestamp = 1, .has_seq = true, .seq = 0, .metrics = m, .metric_count = 2};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_EQUAL(2, dec.count);
    TEST_ASSERT_EQUAL(SPB_VAL_TEMPLATE, dec.m[0].kind);
    TEST_ASSERT_FALSE(dec.m[0].has_alias);
    TEST_ASSERT_TRUE(dec.m[0].is_definition);
    TEST_ASSERT_FALSE(dec.m[0].has_ref);
    TEST_ASSERT_EQUAL(2, dec.m[0].members);
    TEST_ASSERT_TRUE(dec.m[1].has_alias);
    TEST_ASSERT_FALSE(dec.m[1].is_definition);
    TEST_ASSERT_TRUE(dec.m[1].has_ref);
    TEST_ASSERT_EQUAL_STRING("Meter", dec.m[1].ref);
    TEST_ASSERT_EQUAL(4, dec.mem_count);
    TEST_ASSERT_EQUAL_STRING("Voltage", dec.mem[0].name);
    TEST_ASSERT_FALSE(dec.mem[0].has_alias);
    TEST_ASSERT_EQUAL_STRING("V", dec.mem[0].eng_unit);
    TEST_ASSERT_EQUAL(SPB_DT_BOOLEAN, dec.mem[1].datatype);
    TEST_ASSERT_EQUAL_DOUBLE(230.5, dec.mem[2].num);
    TEST_ASSERT_EQUAL_INT32(SPB_QUALITY_GOOD, dec.mem[2].quality);
    TEST_ASSERT_TRUE(dec.mem[3].is_null);

    /* one level only: a member that is a template fails the encoding */
    const spb_metric_t nested = {.name = "x", .no_alias = true, .datatype = SPB_DT_TEMPLATE,
                                 .v.tmpl = &inst};
    const spb_template_t outer = {.ref = "Meter", .count = 1, .members = &nested};
    m[1].v.tmpl = &outer;
    TEST_ASSERT_EQUAL(0, spb_encode(&p, buf, sizeof(buf)));
}

static void test_worst_case_birth_fits(void)
{
    /* SPB-16: the widest DBIRTH metric carries every property; 64 of them, either numeric with
     * an engineering range or 32-character strings, with 39-character names and units */
    static spb_metric_t m[GW_MAX_TAGS_PER_DEVICE];
    static const char text[] = "0123456789abcdef0123456789abcdef";
    char name[40], units[GW_UNITS_SIZE];
    memset(name, 'x', 39);
    name[39] = '\0';
    memset(units, 'u', GW_UNITS_SIZE - 1);
    units[GW_UNITS_SIZE - 1] = '\0';
    for (int kind = 0; kind < 2; kind++) {
        for (int i = 0; i < GW_MAX_TAGS_PER_DEVICE; i++) {
            m[i] = (spb_metric_t){.name = name, .alias = 100 + 255, .timestamp = 1900000000000ull,
                                  .props = {.eng_unit = units, .has_quality = true,
                                            .quality = SPB_QUALITY_GOOD}};
            if (kind == 0) {
                m[i].datatype = SPB_DT_DOUBLE;
                m[i].v.num = -1.0 / 3.0;
                m[i].props.has_eng_low = m[i].props.has_eng_high = true;
                m[i].props.eng_low = -3e38f;
                m[i].props.eng_high = 3e38f;
            } else {
                m[i].datatype = SPB_DT_STRING;
                m[i].v.str = text;
            }
        }
        spb_payload_t p = {.timestamp = 1900000000000ull, .has_seq = true, .seq = 255,
                           .metrics = m, .metric_count = GW_MAX_TAGS_PER_DEVICE};
        size_t len = spb_encode(&p, buf, sizeof(buf));
        printf("worst-case DBIRTH (%s): %u bytes of %u\n", kind == 0 ? "Double" : "String",
               (unsigned)len, (unsigned)GW_SPB_PAYLOAD_MAX);
        TEST_ASSERT_GREATER_THAN(0, len);
        TEST_ASSERT_LESS_THAN(GW_SPB_PAYLOAD_MAX, len);
        TEST_ASSERT_EQUAL(0, spb_encode(&p, buf, len - 1));  /* overflow detected, not truncated */
    }
}

/* Issue #87: a payload with one String metric "s" of @p n characters, built by hand (the
 * encoder itself refuses what does not fit). */
static size_t raw_string_payload(uint8_t *out, size_t n)
{
    uint8_t metric[400];
    size_t k = 0;
    metric[k++] = 0x0a;                 /* name */
    metric[k++] = 1;
    metric[k++] = 's';
    metric[k++] = 0x20;                 /* datatype */
    metric[k++] = SPB_DT_STRING;
    metric[k++] = 0x7a;                 /* string_value (field 15, length-delimited) */
    metric[k++] = (uint8_t)(0x80 | (n & 0x7f));
    metric[k++] = (uint8_t)(n >> 7);
    memset(metric + k, 'a', n);
    k += n;
    size_t j = 0;
    out[j++] = 0x12;                    /* metrics */
    out[j++] = (uint8_t)(0x80 | (k & 0x7f));
    out[j++] = (uint8_t)(k >> 7);
    memcpy(out + j, metric, k);
    return j + k;
}

/* Issue #87: String values up to SPB_STRING_MAX (256) both ways; one more is refused: the
 * encoder fails instead of cutting it, the decoder rejects the payload and says why. */
static void test_string_limit(void)
{
    char s[SPB_STRING_MAX + 2];
    memset(s, 'u', sizeof(s));
    s[SPB_STRING_MAX] = '\0';
    spb_metric_t m = {.name = "Node Control/Firmware Update", .alias = 40,
                      .datatype = SPB_DT_STRING, .v.str = s};
    spb_payload_t p = {.timestamp = 1, .metrics = &m, .metric_count = 1};
    size_t len = spb_encode(&p, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_TRUE(decode_payload(buf, len, &dec));
    TEST_ASSERT_EQUAL(SPB_STRING_MAX, strlen(dec.m[0].str));
    TEST_ASSERT_EQUAL_STRING(s, dec.m[0].str);
    s[SPB_STRING_MAX] = 'u';
    s[SPB_STRING_MAX + 1] = '\0';
    TEST_ASSERT_EQUAL(0, spb_encode(&p, buf, sizeof(buf)));   /* 257: refused, not cut */

    len = raw_string_payload(buf, SPB_STRING_MAX);
    TEST_ASSERT_TRUE(spb_decode(buf, len, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("", spb_last_error());
    len = raw_string_payload(buf, SPB_STRING_MAX + 1);
    TEST_ASSERT_FALSE(spb_decode(buf, len, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("string overflow", spb_last_error());
}

static void test_topics(void)
{
    char t[GW_TOPIC_MAX];
    TEST_ASSERT_TRUE(spb_topic(t, sizeof(t), "Plant1", "NBIRTH", "GW01", NULL));
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/NBIRTH/GW01", t);
    TEST_ASSERT_TRUE(spb_topic(t, sizeof(t), "Plant1", "DDATA", "GW01", "Meter1"));
    TEST_ASSERT_EQUAL_STRING("spBv1.0/Plant1/DDATA/GW01/Meter1", t);
    TEST_ASSERT_FALSE(spb_topic(t, 10, "Plant1", "DDATA", "GW01", "Meter1"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_all_types);
    RUN_TEST(test_round_trip_64bit_exact);
    RUN_TEST(test_wire_format_int16_sign_extension);
    RUN_TEST(test_ndeath_has_no_seq);
    RUN_TEST(test_rx_number_variants);
    RUN_TEST(test_decode_rejects_garbage);
    RUN_TEST(test_properties_round_trip);
    RUN_TEST(test_property_decode_bounds);
    RUN_TEST(test_bytes_and_file);
    RUN_TEST(test_dataset_round_trip);
    RUN_TEST(test_template_round_trip);
    RUN_TEST(test_worst_case_birth_fits);
    RUN_TEST(test_string_limit);
    RUN_TEST(test_topics);
    return UNITY_END();
}
