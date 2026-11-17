/* QA-1: the configuration hash of the store & forward spill (SPB-14, #145): an explicit list of
 * the fields that give a stored record its meaning, never the C layout of the structures.
 *
 * Built twice: test_sfcfg, and test_sfcfg_packed with -fpack-struct -fshort-enums (no padding,
 * 1-byte enums: the structures have another size and other offsets). Both must give the same
 * golden value from the same document. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "gw_config.h"
#include "gw_conv.h"
#include "gw_sfspill.h"
#include "spb.h"
#include "unity.h"

static gw_config_t base, var;
static gw_cfg_err_t err;

/* Two devices, one with a template; the tags cover each value encoding of the records (double,
 * a scaled tag, UInt64 bits, a string, Bytes, Boolean, a scaled Int64). */
#define DOC_HEAD "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"},"
#define DEV_M1                                                                                    \
    "{\"name\":\"M1\",\"unit_id\":1,\"poll_ms\":1000,\"tags\":["                                  \
    "{\"name\":\"V\",\"area\":\"input\",\"address\":0,\"type\":\"float32\",\"deadband\":0.1},"    \
    "{\"name\":\"S\",\"area\":\"holding\",\"address\":10,\"type\":\"int16\",\"scale\":0.1},"      \
    "{\"name\":\"C\",\"area\":\"holding\",\"address\":20,\"type\":\"uint64\"},"                   \
    "{\"name\":\"T\",\"area\":\"holding\",\"address\":30,\"type\":\"string\",\"length\":4},"      \
    "{\"name\":\"B\",\"area\":\"holding\",\"address\":40,\"type\":\"bytes\",\"length\":2}]}"
#define DEV_P2                                                                                    \
    "{\"name\":\"P2\",\"type\":\"Pump\",\"unit_id\":2,\"tags\":["                                 \
    "{\"name\":\"On\",\"area\":\"coil\",\"address\":0,\"type\":\"bool\"},"                        \
    "{\"name\":\"N\",\"area\":\"holding\",\"address\":1,\"type\":\"int64\",\"scale\":2}]}"
static const char k_doc[] = DOC_HEAD "\"devices\":[" DEV_M1 "," DEV_P2 "]}";

/* The golden value of k_doc (format 3). It changes only with the canonical list: then also
 * increment GW_SFCFG_HASH_FORMAT in gw_sfcfg.c (a new ring on every card after the update). */
#define GOLDEN 0xCC15B152u

void setUp(void)
{
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(k_doc, strlen(k_doc), &base, &err), err.msg);
    var = base;
}

void tearDown(void) {}

static uint32_t parse_hash(const char *doc)
{
    static gw_config_t c;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(doc, strlen(doc), &c, &err), err.msg);
    return gw_sfspill_cfg_hash(&c);
}

/* ---- an independent writer of the documented input (gw_sfcfg.c, "Canonical input") ---- */

static uint8_t ref[4096];
static size_t ref_len;

static void put(const void *p, size_t n)
{
    TEST_ASSERT_TRUE(ref_len + n <= sizeof(ref));
    memcpy(ref + ref_len, p, n);
    ref_len += n;
}

static void put_le(uint64_t v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        put(&b, 1);
    }
}

static void put_str(const char *s)
{
    put_le(strlen(s), 2);
    put(s, strlen(s));
}

static void put_tag(const char *name, uint32_t dt, uint8_t bytes_len)
{
    put_str(name);
    put_le(dt, 4);
    put_le(bytes_len, 1);
}

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void test_golden_value_and_documented_input(void)
{
    ref_len = 0;
    put_le(3, 4);   /* format */
    put_str("G");
    put_str("E");
    put_str("");    /* sparkplug.device_prefix (#172) */
    put_le(0, 1);   /* sparkplug.naming (#174): "sparkplug" */
    put_le(2, 2);   /* devices */
    put_str("M1");
    put_str("");
    put_le(0, 2);
    put_le(5, 2);
    put_str("P2");
    put_str("Pump");
    put_le(5, 2);
    put_le(2, 2);
    put_le(7, 2);   /* tags */
    put_tag("V", SPB_DT_FLOAT, 0);
    put_tag("S", SPB_DT_FLOAT, 0);      /* int16 with a scale */
    put_tag("C", SPB_DT_UINT64, 0);
    put_tag("T", SPB_DT_STRING, 0);     /* a String's length is left out */
    put_tag("B", SPB_DT_BYTES, 2);
    put_tag("On", SPB_DT_BOOLEAN, 0);
    put_tag("N", SPB_DT_DOUBLE, 0);     /* int64 with a scale */
    uint32_t h = gw_sfspill_cfg_hash(&base);
    printf("cfg hash %08lx (sizeof gw_tag_cfg_t %u, gw_device_cfg_t %u)\n", (unsigned long)h,
           (unsigned)sizeof(gw_tag_cfg_t), (unsigned)sizeof(gw_device_cfg_t));
    TEST_ASSERT_EQUAL_HEX32(fnv1a(ref, ref_len), h);
    TEST_ASSERT_EQUAL_HEX32(GOLDEN, h);
}

/* The two builds really have different layouts. */
static void test_layout_of_this_build(void)
{
#ifdef GW_SFCFG_PACKED
    TEST_ASSERT_EQUAL_size_t(1, sizeof(gw_type_t));
    TEST_ASSERT_EQUAL_size_t(offsetof(gw_tag_cfg_t, has_write_max) + 1, offsetof(gw_tag_cfg_t, scale));
#else
    TEST_ASSERT_EQUAL_size_t(sizeof(int), sizeof(gw_type_t));
    TEST_ASSERT_EQUAL_size_t(0, offsetof(gw_tag_cfg_t, scale) % _Alignof(double));
#endif
}

#define SAME(stmt)                                                                     \
    do {                                                                               \
        var = base;                                                                    \
        stmt;                                                                          \
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(GOLDEN, gw_sfspill_cfg_hash(&var), #stmt);     \
    } while (0)

#define DIFFERENT(stmt)                                                                \
    do {                                                                               \
        var = base;                                                                    \
        stmt;                                                                          \
        TEST_ASSERT_NOT_EQUAL_MESSAGE(GOLDEN, gw_sfspill_cfg_hash(&var), #stmt);       \
    } while (0)

/* (a) fields outside the list: the stored value is the engineering value, and the replay never
 * reads them */
static void test_excluded_fields_keep_the_hash(void)
{
    gw_tag_cfg_t *v = &var.tags[0], *s = &var.tags[1], *t = &var.tags[3], *n = &var.tags[6];
    gw_device_cfg_t *d = &var.devices[0];
    SAME(v->area = GW_AREA_HOLDING);
    SAME(v->address = 99);
    SAME(v->word_order = GW_WORD_DCBA);
    SAME(var.tags[5].bit = 3);
    SAME(v->slow = true);
    SAME(v->writable = true);
    SAME(v->deadband = 5.0);
    SAME(strcpy(v->units, "kV"));
    SAME((v->has_eng_low = true, v->eng_low = -1.0f));
    SAME((v->has_eng_high = true, v->eng_high = 1.0f));
    SAME(s->scale = 0.01);       /* still scaled: still Float */
    SAME(s->offset = 4.0);
    SAME(n->scale = 3.0);        /* still Double */
    SAME(t->length = 16);        /* a String record holds its own length */
    SAME(v->scale = 2.0);        /* a float32 stays Float */
    SAME(s->type = GW_TYPE_UINT16); /* scaled: Float either way, the record holds the value */
    SAME(d->unit_id = 9);
    SAME(d->poll_ms = 250);
    SAME(d->slow_poll_ms = 60000);
    SAME(d->offline_after = 7);
    SAME(d->block_reads = 0);
    SAME(d->max_gap = 20);
    SAME(strcpy(var.mqtt.host, "other"));
    SAME(var.mqtt.port = 8883);
    SAME(strcpy(var.sparkplug.primary_host_id, "SCADA"));
    SAME(var.sparkplug.sf_sd_max_kb = 64);
    SAME(var.rs485.baud = 19200);
    /* the bytes after the NUL of a name are never read */
    SAME(memset(v->name + 2, 0xFF, sizeof(v->name) - 2));
    SAME(memset(d->name + 3, 0x5A, sizeof(d->name) - 3));
    SAME(memset(var.sparkplug.edge_node_id + 2, 0x11, sizeof(var.sparkplug.edge_node_id) - 2));
    /* the same document with other acquisition settings */
    TEST_ASSERT_EQUAL_HEX32(GOLDEN, parse_hash(
        DOC_HEAD "\"rs485\":{\"baud\":19200},\"devices\":["
        "{\"name\":\"M1\",\"unit_id\":7,\"poll_ms\":200,\"slow_poll_ms\":9000,\"offline_after\":9,"
        "\"block_reads\":false,\"tags\":["
        "{\"name\":\"V\",\"area\":\"holding\",\"address\":5,\"type\":\"float32\",\"deadband\":3,"
        "\"word_order\":\"little\",\"units\":\"V\",\"eng_low\":0,\"eng_high\":400,\"scan\":\"slow\"},"
        "{\"name\":\"S\",\"area\":\"holding\",\"address\":11,\"type\":\"int16\",\"scale\":0.5,"
        "\"offset\":1,\"writable\":true},"
        "{\"name\":\"C\",\"area\":\"input\",\"address\":21,\"type\":\"uint64\"},"
        "{\"name\":\"T\",\"area\":\"holding\",\"address\":30,\"type\":\"string\",\"length\":8},"
        "{\"name\":\"B\",\"area\":\"input\",\"address\":40,\"type\":\"bytes\",\"length\":2}]},"
        DEV_P2 "]}"));
}

/* (b) each field of the list */
static void test_included_fields_change_the_hash(void)
{
    gw_tag_cfg_t *v = &var.tags[0], *s = &var.tags[1], *b = &var.tags[4], *n = &var.tags[6];
    DIFFERENT(strcpy(v->name, "W"));
    DIFFERENT(strcpy(var.tags[6].name, "N2"));            /* a template member */
    DIFFERENT(v->type = GW_TYPE_FLOAT64);                  /* Float -> Double */
    DIFFERENT(var.tags[2].type = GW_TYPE_INT64);           /* UInt64 -> Int64 */
    DIFFERENT((s->scale = 1.0, s->offset = 0.0));          /* Float -> Int16 */
    DIFFERENT((n->scale = 1.0));                           /* Double -> Int64: other encoding */
    DIFFERENT(b->length = 3);                              /* Bytes: 2 x length on replay */
    DIFFERENT(var.tags[3].type = GW_TYPE_BYTES);           /* String -> Bytes */
    DIFFERENT(strcpy(var.devices[0].name, "M9"));          /* the device_id */
    DIFFERENT(strcpy(var.devices[1].type, "Valve"));       /* the template instance */
    DIFFERENT(var.devices[1].type[0] = '\0');              /* no template */
    DIFFERENT(strcpy(var.sparkplug.group_id, "G2"));
    DIFFERENT(strcpy(var.sparkplug.edge_node_id, "E2"));
    DIFFERENT(strcpy(var.sparkplug.device_prefix, "acme:line3"));   /* #172: each device_id */
    DIFFERENT(var.sparkplug.naming = GW_NAMING_UMH);   /* #174: no template instance on replay */
    DIFFERENT(var.device_count = 1);
    DIFFERENT(var.tag_count = 6);
    DIFFERENT((var.devices[0].tag_count = 4, var.devices[1].first_tag = 4,
               var.devices[1].tag_count = 3));             /* a tag moves to the next device */
    /* order: two tags swapped, two devices swapped, a tag added */
    const char swapped_tags[] = DOC_HEAD "\"devices\":["
        "{\"name\":\"M1\",\"unit_id\":1,\"poll_ms\":1000,\"tags\":["
        "{\"name\":\"S\",\"area\":\"holding\",\"address\":10,\"type\":\"int16\",\"scale\":0.1},"
        "{\"name\":\"V\",\"area\":\"input\",\"address\":0,\"type\":\"float32\",\"deadband\":0.1},"
        "{\"name\":\"C\",\"area\":\"holding\",\"address\":20,\"type\":\"uint64\"},"
        "{\"name\":\"T\",\"area\":\"holding\",\"address\":30,\"type\":\"string\",\"length\":4},"
        "{\"name\":\"B\",\"area\":\"holding\",\"address\":40,\"type\":\"bytes\",\"length\":2}]},"
        DEV_P2 "]}";
    const char swapped_devices[] = DOC_HEAD "\"devices\":[" DEV_P2 "," DEV_M1 "]}";
    const char added_tag[] = DOC_HEAD "\"devices\":[" DEV_M1 ","
        "{\"name\":\"P2\",\"type\":\"Pump\",\"unit_id\":2,\"tags\":["
        "{\"name\":\"On\",\"area\":\"coil\",\"address\":0,\"type\":\"bool\"},"
        "{\"name\":\"N\",\"area\":\"holding\",\"address\":1,\"type\":\"int64\",\"scale\":2},"
        "{\"name\":\"X\",\"area\":\"holding\",\"address\":9,\"type\":\"uint16\"}]}]}";
    uint32_t h1 = parse_hash(swapped_tags), h2 = parse_hash(swapped_devices),
             h3 = parse_hash(added_tag);
    TEST_ASSERT_NOT_EQUAL(GOLDEN, h1);
    TEST_ASSERT_NOT_EQUAL(GOLDEN, h2);
    TEST_ASSERT_NOT_EQUAL(GOLDEN, h3);
    TEST_ASSERT_NOT_EQUAL(h1, h2);
    /* lengths are in the input: "AB"+"C" is not "A"+"BC" */
    var = base;
    strcpy(var.devices[0].name, "M1P");
    strcpy(var.devices[0].type, "");
    gw_config_t other = var;
    strcpy(other.devices[0].name, "M");
    strcpy(other.devices[0].type, "1P");
    TEST_ASSERT_NOT_EQUAL(gw_sfspill_cfg_hash(&var), gw_sfspill_cfg_hash(&other));
}

/* The hash feeds the Sparkplug datatype and not the value kind: that is enough only while the
 * kind (how gw_node reads the record's value union) is a function of the datatype. */
static void test_value_kind_follows_the_datatype(void)
{
    int kind_of[32];
    for (size_t i = 0; i < 32; i++) {
        kind_of[i] = -1;
    }
    for (int type = GW_TYPE_BOOL; type <= GW_TYPE_BYTES; type++) {
        for (int scaled = 0; scaled < 2; scaled++) {
            gw_tag_cfg_t t;
            memset(&t, 0, sizeof(t));
            t.type = (gw_type_t)type;
            t.scale = scaled ? 2.0 : 1.0;
            t.length = (type == GW_TYPE_STRING || type == GW_TYPE_BYTES) ? 2 : 0;
            uint32_t dt = gw_conv_datatype(&t);
            int kind = (int)gw_conv_kind(&t);
            TEST_ASSERT_TRUE(dt < 32);
            if (kind_of[dt] < 0) {
                kind_of[dt] = kind;
            }
            char msg[64];
            snprintf(msg, sizeof(msg), "type %d scaled %d datatype %lu", type, scaled,
                     (unsigned long)dt);
            TEST_ASSERT_EQUAL_INT_MESSAGE(kind_of[dt], kind, msg);
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_golden_value_and_documented_input);
    RUN_TEST(test_layout_of_this_build);
    RUN_TEST(test_excluded_fields_keep_the_hash);
    RUN_TEST(test_included_fields_change_the_hash);
    RUN_TEST(test_value_kind_follows_the_datatype);
    return UNITY_END();
}
