/* QA-1: configuration parsing and validation (CFG-1, SPEC section 6). */
#include <stdio.h>
#include <string.h>

#include "gw_config.h"
#include "support.h"
#include "unity.h"

static gw_config_t cfg;
static gw_cfg_err_t err;

void setUp(void) {}
void tearDown(void) {}

static bool parse(const char *json)
{
    return gw_config_parse(json, strlen(json), &cfg, &err);
}

/* Minimal valid document with one device whose single tag is @p tag_json. */
static bool parse_tag(const char *tag_json)
{
    char doc[1024];
    snprintf(doc, sizeof(doc),
             "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"},"
             "\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[%s]}]}",
             tag_json);
    return parse(doc);
}

#define ASSERT_REJECTED(ok, fragment)                                                     \
    do {                                                                                  \
        TEST_ASSERT_FALSE_MESSAGE(ok, "document should be rejected");                     \
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err.msg, fragment), err.msg);                 \
    } while (0)

static void test_spec_example(void)
{
    size_t len;
    const char *json = support_read_file(GW_SPEC_EXAMPLE, &len);
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(json, len, &cfg, &err), err.msg);
    TEST_ASSERT_TRUE(cfg.network.dhcp);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){192, 168, 1, 50}), cfg.network.ip, 4);
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", cfg.ntp.server);
    TEST_ASSERT_EQUAL_STRING("192.168.1.10", cfg.mqtt.host);
    TEST_ASSERT_EQUAL(1883, cfg.mqtt.port);
    TEST_ASSERT_EQUAL_STRING("GW01", cfg.mqtt.client_id);  /* empty client_id -> edge_node_id */
    TEST_ASSERT_EQUAL(30, cfg.mqtt.keepalive_s);
    TEST_ASSERT_EQUAL_STRING("Plant1", cfg.sparkplug.group_id);
    TEST_ASSERT_EQUAL_STRING("GW01", cfg.sparkplug.edge_node_id);
    TEST_ASSERT_EQUAL(9600, cfg.rs485.baud);
    TEST_ASSERT_EQUAL(GW_PARITY_NONE, cfg.rs485.parity);
    TEST_ASSERT_EQUAL(1, cfg.rs485.stop_bits);
    TEST_ASSERT_EQUAL(200, cfg.rs485.response_timeout_ms);
    TEST_ASSERT_EQUAL(2, cfg.rs485.retries);
    TEST_ASSERT_EQUAL(5, cfg.rs485.inter_frame_delay_ms);
    TEST_ASSERT_EQUAL(20, cfg.local_io.di_debounce_ms);
    TEST_ASSERT_EQUAL(1, cfg.device_count);
    TEST_ASSERT_EQUAL(3, cfg.tag_count);
    const gw_device_cfg_t *d = &cfg.devices[0];
    TEST_ASSERT_EQUAL_STRING("Meter1", d->name);
    TEST_ASSERT_EQUAL(1, d->unit_id);
    TEST_ASSERT_EQUAL(1000, d->poll_ms);
    TEST_ASSERT_EQUAL(3, d->offline_after);
    const gw_tag_cfg_t *v = &cfg.tags[0];
    TEST_ASSERT_EQUAL_STRING("Voltage", v->name);
    TEST_ASSERT_EQUAL(GW_AREA_INPUT, v->area);
    TEST_ASSERT_EQUAL(GW_TYPE_FLOAT32, v->type);
    TEST_ASSERT_EQUAL(GW_WORD_BIG, v->word_order);
    TEST_ASSERT_EQUAL_DOUBLE(0.1, v->deadband);
    TEST_ASSERT_EQUAL_STRING("V", v->units);
    const gw_tag_cfg_t *sp = &cfg.tags[1];
    TEST_ASSERT_EQUAL(GW_AREA_HOLDING, sp->area);
    TEST_ASSERT_EQUAL(10, sp->address);
    TEST_ASSERT_EQUAL_DOUBLE(0.1, sp->scale);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, sp->offset);   /* default */
    TEST_ASSERT_TRUE(sp->writable);
    TEST_ASSERT_EQUAL(GW_TYPE_BOOL, cfg.tags[2].type);
    TEST_ASSERT_EQUAL(2, gw_config_find_tag(&cfg, 0, "Run"));
    TEST_ASSERT_EQUAL(-1, gw_config_find_tag(&cfg, 0, "Nope"));
    TEST_ASSERT_EQUAL(0, gw_config_find_device(&cfg, "Meter1", 6));
    TEST_ASSERT_EQUAL(-1, gw_config_find_device(&cfg, "Meter", 5));
}

static void test_embedded_default_is_valid(void)
{
    size_t len;
    const char *json = support_read_file(GW_DEFAULT_CONFIG, &len);
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(json, len, &cfg, &err), err.msg);
}

static void test_defaults_and_static_ip(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        parse("{\"network\":{\"dhcp\":false,\"ip\":\"10.0.0.2\",\"netmask\":\"255.0.0.0\","
              "\"gateway\":\"10.0.0.1\"},\"mqtt\":{\"host\":\"b\",\"client_id\":\"cid\"},"
              "\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"}}"),
        err.msg);
    TEST_ASSERT_FALSE(cfg.network.dhcp);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){10, 0, 0, 1}), cfg.network.gateway, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){0, 0, 0, 0}), cfg.network.dns, 4);
    TEST_ASSERT_EQUAL_STRING("cid", cfg.mqtt.client_id);
    TEST_ASSERT_EQUAL(1883, cfg.mqtt.port);
    TEST_ASSERT_EQUAL(9600, cfg.rs485.baud);
    TEST_ASSERT_EQUAL(20, cfg.local_io.di_debounce_ms);
    TEST_ASSERT_EQUAL(0, cfg.device_count);
}

static void test_schema_rules(void)
{
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"bool\"}"),
                    "devices[0].tags[0].type");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"coil\",\"address\":0,\"type\":\"int16\"}"),
                    "solo admiten type=bool");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"writable\":true}"),
                    "devices[0].tags[0].writable");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"discrete\",\"address\":0,\"type\":\"bool\","
                              "\"writable\":true}"),
                    "writable");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"inputs\",\"address\":0,\"type\":\"int16\"}"),
                    "coil|discrete|holding|input");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int64\"}"),
                    "bool|uint16|int16|uint32|int32|float32");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":65535,\"type\":\"int32\"}"),
                    "65535");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":1.5,\"type\":\"int16\"}"),
                    "entero");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"scale\":0}"),
                    "scale");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"word_order\":\"middle\"}"),
                    "big|little");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"adress\":0,\"type\":\"int16\"}"),
                    "clave desconocida 'adress'");
    ASSERT_REJECTED(parse_tag("{\"area\":\"input\",\"address\":0,\"type\":\"int16\"}"),
                    "devices[0].tags[0].name: campo obligatorio");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\"},"
                              "{\"name\":\"t\",\"area\":\"input\",\"address\":1,\"type\":\"int16\"}"),
                    "duplicado");
    TEST_ASSERT_TRUE_MESSAGE(
        parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":65534,\"type\":\"float32\","
                  "\"word_order\":\"little\",\"deadband\":0.5,\"units\":\"kWh\"}"),
        err.msg);
}

static void test_section_errors(void)
{
    ASSERT_REJECTED(parse("{\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"}}"), "mqtt:");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G/1\","
                          "\"edge_node_id\":\"E\"}}"),
                    "sparkplug.group_id");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\",\"port\":70000},\"sparkplug\":{\"group_id\":"
                          "\"G\",\"edge_node_id\":\"E\"}}"),
                    "mqtt.port: valor 70000 fuera de rango");
    ASSERT_REJECTED(parse("{\"network\":{\"dhcp\":false,\"ip\":\"10.0.0.256\",\"netmask\":"
                          "\"255.0.0.0\",\"gateway\":\"10.0.0.1\"},\"mqtt\":{\"host\":\"b\"},"
                          "\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"}}"),
                    "network.ip");
    ASSERT_REJECTED(parse("{\"network\":{\"dhcp\":false},\"mqtt\":{\"host\":\"b\"},"
                          "\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"}}"),
                    "network.ip: campo obligatorio");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"rs485\":{\"parity\":\"mark\"}}"),
                    "rs485.parity");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"rs485\":{\"baud\":250000}}"),
                    "rs485.baud");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"extra\":1}"),
                    "clave desconocida 'extra'");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\",}"), "JSON mal formado");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"devices\":[{\"name\":\"D\",\"unit_id\":248,"
                          "\"tags\":[]}]}"),
                    "devices[0].unit_id");
}

static void test_limits(void)
{
    /* 65 tags in one device */
    static char doc[64 * 1024];
    int n = snprintf(doc, sizeof(doc),
                     "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":"
                     "\"E\"},\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[");
    for (int i = 0; i < 65; i++) {
        n += snprintf(doc + n, sizeof(doc) - (size_t)n,
                      "%s{\"name\":\"t%d\",\"area\":\"holding\",\"address\":%d,\"type\":\"uint16\"}",
                      i ? "," : "", i, i);
    }
    snprintf(doc + n, sizeof(doc) - (size_t)n, "]}]}");
    ASSERT_REJECTED(parse(doc), "máximo por dispositivo es 64");

    /* 17 devices */
    n = snprintf(doc, sizeof(doc),
                 "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":"
                 "\"E\"},\"devices\":[");
    for (int d = 0; d < 17; d++) {
        n += snprintf(doc + n, sizeof(doc) - (size_t)n,
                      "%s{\"name\":\"D%d\",\"unit_id\":%d,\"tags\":[{\"name\":\"t\",\"area\":"
                      "\"coil\",\"address\":0,\"type\":\"bool\"}]}",
                      d ? "," : "", d, d + 1);
    }
    snprintf(doc + n, sizeof(doc) - (size_t)n, "]}");
    ASSERT_REJECTED(parse(doc), "máximo es 16");

    /* 5 devices x 64 tags = 320 > 256 total */
    n = snprintf(doc, sizeof(doc),
                 "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":"
                 "\"E\"},\"devices\":[");
    for (int d = 0; d < 5; d++) {
        n += snprintf(doc + n, sizeof(doc) - (size_t)n, "%s{\"name\":\"D%d\",\"unit_id\":1,\"tags\":[",
                      d ? "," : "", d);
        for (int i = 0; i < 64; i++) {
            n += snprintf(doc + n, sizeof(doc) - (size_t)n,
                          "%s{\"name\":\"t%d\",\"area\":\"input\",\"address\":%d,\"type\":\"int16\"}",
                          i ? "," : "", i, i);
        }
        n += snprintf(doc + n, sizeof(doc) - (size_t)n, "]}");
    }
    snprintf(doc + n, sizeof(doc) - (size_t)n, "]}");
    ASSERT_REJECTED(parse(doc), "devices[4].tags: se supera el máximo total de 256");
    TEST_ASSERT_EQUAL(0, cfg.tag_count);  /* zeroed on failure */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_spec_example);
    RUN_TEST(test_embedded_default_is_valid);
    RUN_TEST(test_defaults_and_static_ip);
    RUN_TEST(test_schema_rules);
    RUN_TEST(test_section_errors);
    RUN_TEST(test_limits);
    return UNITY_END();
}
