/* QA-1: configuration parsing and validation (CFG-1, SPEC section 6). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_config.h"
#include "gw_sfspill.h"
#include "gw_utf8.h"
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
    /* issue #13: out of range is a sentinel, not device 0 */
    TEST_ASSERT_EQUAL(0, gw_config_tag_device(&cfg, 2));
    TEST_ASSERT_EQUAL(GW_NO_DEVICE, gw_config_tag_device(&cfg, 3));
    TEST_ASSERT_EQUAL(GW_NO_DEVICE, gw_config_tag_device(&cfg, UINT16_MAX));
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

/* Document with @p network_json as the "network" section. */
static bool parse_network(const char *network_json)
{
    char doc[1024];
    snprintf(doc, sizeof(doc),
             "{\"network\":%s,\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
             "\"edge_node_id\":\"E\"}}",
             network_json);
    return parse(doc);
}

/* NET-4: optional "network.wifi" section. */
static void test_wifi_section(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse_network("{\"dhcp\":true}"), err.msg);
    TEST_ASSERT_FALSE(cfg.network.wifi.enabled);   /* absent = Ethernet only, as in the MVP */
    TEST_ASSERT_EQUAL(GW_WIFI_BACKUP, cfg.network.wifi.role);
    TEST_ASSERT_EQUAL(30, cfg.network.wifi.failback_s);
    TEST_ASSERT_TRUE(cfg.network.wifi.ipv4.dhcp);

    TEST_ASSERT_TRUE_MESSAGE(
        parse_network("{\"wifi\":{\"enabled\":true,\"ssid\":\"Plant WLAN\",\"password\":"
                      "\"12345678\",\"role\":\"primary\",\"failback_s\":0,\"dhcp\":false,"
                      "\"ip\":\"10.1.0.5\",\"netmask\":\"255.255.0.0\",\"gateway\":\"10.1.0.1\","
                      "\"dns\":\"10.1.0.2\"}}"),
        err.msg);
    TEST_ASSERT_TRUE(cfg.network.dhcp);   /* the Ethernet keys keep their defaults */
    TEST_ASSERT_TRUE(cfg.network.wifi.enabled);
    TEST_ASSERT_EQUAL_STRING("Plant WLAN", cfg.network.wifi.ssid);
    TEST_ASSERT_EQUAL_STRING("12345678", cfg.network.wifi.password);
    TEST_ASSERT_EQUAL(GW_WIFI_PRIMARY, cfg.network.wifi.role);
    TEST_ASSERT_EQUAL(0, cfg.network.wifi.failback_s);
    TEST_ASSERT_FALSE(cfg.network.wifi.ipv4.dhcp);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){10, 1, 0, 5}), cfg.network.wifi.ipv4.ip, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){10, 1, 0, 2}), cfg.network.wifi.ipv4.dns, 4);

    /* open network and a raw 64-hex-digit PSK */
    TEST_ASSERT_TRUE_MESSAGE(parse_network("{\"wifi\":{\"enabled\":true,\"ssid\":\"open\"}}"),
                             err.msg);
    TEST_ASSERT_TRUE_MESSAGE(
        parse_network("{\"wifi\":{\"enabled\":true,\"ssid\":\"s\",\"password\":\"0123456789"
                      "abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef\"}}"),
        err.msg);
    /* disabled: an empty SSID is fine (the section can stay in the file) */
    TEST_ASSERT_TRUE_MESSAGE(parse_network("{\"wifi\":{\"enabled\":false}}"), err.msg);

    ASSERT_REJECTED(parse_network("{\"wifi\":{\"enabled\":true}}"),
                    "network.wifi.ssid: obligatorio con enabled=true");
    ASSERT_REJECTED(parse_network("{\"wifi\":{\"enabled\":true,\"ssid\":"
                                  "\"123456789012345678901234567890123\"}}"),
                    "network.wifi.ssid: texto demasiado largo (33 caracteres, máximo 32)");
    ASSERT_REJECTED(parse_network("{\"wifi\":{\"enabled\":true,\"ssid\":\"s\",\"password\":"
                                  "\"1234567\"}}"),
                    "network.wifi.password");
    ASSERT_REJECTED(parse_network("{\"wifi\":{\"enabled\":true,\"ssid\":\"s\",\"password\":\"0123456789"
                                  "abcdef0123456789abcdef0123456789abcdef0123456789abcdeg\"}}"),
                    "network.wifi.password");
    ASSERT_REJECTED(parse_network("{\"wifi\":{\"role\":\"tertiary\"}}"),
                    "network.wifi.role: valor no válido (se esperaba backup|primary)");
    ASSERT_REJECTED(parse_network("{\"wifi\":{\"failback_s\":3601}}"),
                    "network.wifi.failback_s: valor 3601 fuera de rango");
    ASSERT_REJECTED(parse_network("{\"wifi\":{\"enabled\":true,\"ssid\":\"s\",\"dhcp\":false}}"),
                    "network.wifi.ip: campo obligatorio");
    ASSERT_REJECTED(parse_network("{\"wifi\":{\"enabled\":true,\"ssid\":\"s\",\"dhcp\":false,"
                                  "\"ip\":\"10.0.0.2\",\"netmask\":\"255.0.0.0\",\"gateway\":\"10.0.0\"}}"),
                    "network.wifi.gateway: '10.0.0' no es una dirección IPv4 válida");
    ASSERT_REJECTED(parse_network("{\"wifi\":{\"sssid\":\"x\"}}"),
                    "network.wifi.sssid: clave desconocida");
    ASSERT_REJECTED(parse_network("{\"wifi\":true}"), "network.wifi: se esperaba un objeto JSON");
}

static void assert_ipv6(const char *text, const uint8_t expected[16])
{
    uint8_t a[16];
    TEST_ASSERT_TRUE_MESSAGE(gw_parse_ipv6(text, a), text);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(expected, a, 16, text);
}

static void test_ipv6_literals(void)
{
    assert_ipv6("2001:db8::1", (const uint8_t[16]){0x20, 0x01, 0x0d, 0xb8, [15] = 1});
    assert_ipv6("2001:0DB8:0000:0000:0000:0000:0000:0001",
                (const uint8_t[16]){0x20, 0x01, 0x0d, 0xb8, [15] = 1});
    assert_ipv6("::", (const uint8_t[16]){0});
    assert_ipv6("::1", (const uint8_t[16]){[15] = 1});
    assert_ipv6("fe80::", (const uint8_t[16]){0xfe, 0x80});
    assert_ipv6("1:2:3:4:5:6:7::", (const uint8_t[16]){0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7});
    assert_ipv6("::ffff:192.0.2.1",
                (const uint8_t[16]){[10] = 0xff, [11] = 0xff, [12] = 192, [13] = 0, [14] = 2, [15] = 1});
    assert_ipv6("fec0::1234", (const uint8_t[16]){0xfe, 0xc0, [14] = 0x12, [15] = 0x34});
    static const char *const bad[] = {
        "", ":", ":::", "1::2::3", "2001:db8::1:", ":2001:db8::1", "12345::1", "g::1",
        "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8::", "::1.2.3", "::1.2.3.256",
        "1:2:3:4:5:6:7:1.2.3.4", "fe80::1%eth0", "2001:db8::1 ",
    };
    uint8_t a[16];
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(gw_parse_ipv6(bad[i], a), bad[i]);
    }
}

/* NET-7: optional "network.ipv6" section. */
static void test_ipv6_section(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse_network("{\"dhcp\":true}"), err.msg);
    TEST_ASSERT_FALSE(cfg.network.ipv6.enabled);   /* absent = IPv4 only, as in the MVP */
    TEST_ASSERT_TRUE(cfg.network.ipv6.slaac);
    TEST_ASSERT_FALSE(cfg.network.ipv6.has_address);
    TEST_ASSERT_FALSE(cfg.network.ipv6.has_dns);

    TEST_ASSERT_TRUE_MESSAGE(
        parse_network("{\"ipv6\":{\"enabled\":true,\"slaac\":false,\"address\":"
                      "\"fd00:1::50\",\"dns\":\"2001:4860:4860::8888\"}}"),
        err.msg);
    TEST_ASSERT_TRUE(cfg.network.ipv6.enabled);
    TEST_ASSERT_FALSE(cfg.network.ipv6.slaac);
    TEST_ASSERT_TRUE(cfg.network.ipv6.has_address);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[16]){0xfd, 0, 0, 1, [15] = 0x50}),
                                  cfg.network.ipv6.address, 16);
    TEST_ASSERT_TRUE(cfg.network.ipv6.has_dns);
    TEST_ASSERT_EQUAL_HEX8(0x88, cfg.network.ipv6.dns[15]);

    ASSERT_REJECTED(parse_network("{\"ipv6\":{\"enabled\":true,\"address\":\"2001:db8::g\"}}"),
                    "network.ipv6.address: '2001:db8::g' no es una dirección IPv6 válida");
    ASSERT_REJECTED(parse_network("{\"ipv6\":{\"address\":\"fe80::1\"}}"),
                    "network.ipv6.address: se esperaba una dirección unicast");
    ASSERT_REJECTED(parse_network("{\"ipv6\":{\"address\":\"ff02::1\"}}"), "network.ipv6.address");
    ASSERT_REJECTED(parse_network("{\"ipv6\":{\"address\":\"::1\"}}"), "network.ipv6.address");
    ASSERT_REJECTED(parse_network("{\"ipv6\":{\"dns\":\"8.8.8.8\"}}"),
                    "network.ipv6.dns: '8.8.8.8' no es una dirección IPv6 válida");
    ASSERT_REJECTED(parse_network("{\"ipv6\":{\"enabled\":1}}"),
                    "network.ipv6.enabled: se esperaba true o false");
    ASSERT_REJECTED(parse_network("{\"ipv6\":{\"prefix_len\":64}}"),
                    "network.ipv6.prefix_len: clave desconocida");
}

/* REL-4: optional "syslog" section. */
static void test_syslog_section(void)
{
    static const char base[] = "\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                               "\"edge_node_id\":\"E\"}";
    char doc[512];
    snprintf(doc, sizeof(doc), "{%s}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL_STRING("", cfg.syslog.host);   /* absent = disabled */
    TEST_ASSERT_EQUAL(514, cfg.syslog.port);
    TEST_ASSERT_EQUAL(GW_LOG_INFO, cfg.syslog.level);

    snprintf(doc, sizeof(doc),
             "{%s,\"syslog\":{\"host\":\"logs.plant.local\",\"port\":5514,\"level\":\"warning\"}}",
             base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL_STRING("logs.plant.local", cfg.syslog.host);
    TEST_ASSERT_EQUAL(5514, cfg.syslog.port);
    TEST_ASSERT_EQUAL(GW_LOG_WARNING, cfg.syslog.level);
    snprintf(doc, sizeof(doc), "{%s,\"syslog\":{\"host\":\"fd00::10\",\"level\":\"debug\"}}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL(GW_LOG_DEBUG, cfg.syslog.level);
    snprintf(doc, sizeof(doc), "{%s,\"syslog\":{\"level\":\"error\"}}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL(GW_LOG_ERROR, cfg.syslog.level);

    snprintf(doc, sizeof(doc), "{%s,\"syslog\":{\"host\":\"h\",\"port\":0}}", base);
    ASSERT_REJECTED(parse(doc), "syslog.port: valor 0 fuera de rango");
    snprintf(doc, sizeof(doc), "{%s,\"syslog\":{\"level\":\"notice\"}}", base);
    ASSERT_REJECTED(parse(doc), "syslog.level: valor no válido (se esperaba error|warning|info|debug)");
    snprintf(doc, sizeof(doc), "{%s,\"syslog\":{\"facility\":16}}", base);
    ASSERT_REJECTED(parse(doc), "syslog.facility: clave desconocida");
    snprintf(doc, sizeof(doc), "{%s,\"syslog\":{\"host\":5}}", base);
    ASSERT_REJECTED(parse(doc), "syslog.host: se esperaba un texto");
}

/* FW-1: "ota" section, optional; updates enabled by default. */
static void test_ota_section(void)
{
    static const char base[] = "\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                               "\"edge_node_id\":\"E\"}";
    char doc[512];
    snprintf(doc, sizeof(doc), "{%s}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_TRUE(cfg.ota.enabled);
    TEST_ASSERT_EQUAL_STRING("", cfg.ota.ca_file);
    snprintf(doc, sizeof(doc), "{%s,\"ota\":{}}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_TRUE(cfg.ota.enabled);
    snprintf(doc, sizeof(doc), "{%s,\"ota\":{\"enabled\":false,\"ca_file\":\"fw-ca.pem\"}}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_FALSE(cfg.ota.enabled);
    TEST_ASSERT_EQUAL_STRING("fw-ca.pem", cfg.ota.ca_file);
    snprintf(doc, sizeof(doc), "{%s,\"ota\":{\"ca_file\":\"../ca.pem\"}}", base);
    ASSERT_REJECTED(parse(doc), "ota.ca_file: '../ca.pem' no es un nombre de archivo válido");
    TEST_ASSERT_EQUAL_STRING("ota.ca_file", err.path);
    snprintf(doc, sizeof(doc), "{%s,\"ota\":{\"enabled\":1}}", base);
    ASSERT_REJECTED(parse(doc), "ota.enabled: se esperaba true o false");
    snprintf(doc, sizeof(doc), "{%s,\"ota\":{\"url\":\"https://x/y\"}}", base);
    ASSERT_REJECTED(parse(doc), "ota.url: clave desconocida");
    snprintf(doc, sizeof(doc), "{%s,\"ota\":true}", base);
    ASSERT_REJECTED(parse(doc), "ota: se esperaba un objeto JSON");
}

/* Document with "mqtt": {@p mqtt_body} and a minimal sparkplug section. */
static bool parse_mqtt(const char *mqtt_body)
{
    char doc[2048];
    snprintf(doc, sizeof(doc),
             "{\"mqtt\":{%s},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"}}",
             mqtt_body);
    return parse(doc);
}

/* MQTT-4: optional "mqtt.tls" section. */
static void test_tls_section(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\""), err.msg);
    TEST_ASSERT_FALSE(cfg.mqtt.tls.enabled);   /* absent = plain TCP, as in the MVP */
    TEST_ASSERT_EQUAL(1883, cfg.mqtt.port);
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt.tls.ca_file);

    /* TLS on: the default port becomes 8883; an explicit port wins */
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\",\"tls\":{\"enabled\":true}"), err.msg);
    TEST_ASSERT_TRUE(cfg.mqtt.tls.enabled);
    TEST_ASSERT_EQUAL(8883, cfg.mqtt.port);
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt.tls.ca_file);       /* the certificate bundle */
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt.tls.server_name);   /* verified against the host */
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"10.0.0.5\",\"port\":9883,\"tls\":{"
                                        "\"enabled\":true,\"ca_file\":\"plant-ca_2.pem\","
                                        "\"server_name\":\"broker.plant.local\"}"),
                             err.msg);
    TEST_ASSERT_EQUAL(9883, cfg.mqtt.port);
    TEST_ASSERT_EQUAL_STRING("plant-ca_2.pem", cfg.mqtt.tls.ca_file);
    TEST_ASSERT_EQUAL_STRING("broker.plant.local", cfg.mqtt.tls.server_name);
    /* the keys are validated also with TLS off (a disabled section stays consistent) */
    ASSERT_REJECTED(parse_mqtt("\"host\":\"b\",\"tls\":{\"ca_file\":\"../ca.pem\"}"),
                    "mqtt.tls.ca_file: '../ca.pem' no es un nombre de archivo válido");

    static const char *const bad_names[] = {"/cfg/ca.pem", "a b.pem", ".hidden", "config.json",
                                            "config.json.tmp", "ca\\\\x.pem",
                                            "0123456789012345678901234567890123.pem"};
    for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
        char body[256];
        snprintf(body, sizeof(body), "\"host\":\"b\",\"tls\":{\"enabled\":true,\"ca_file\":\"%s\"}",
                 bad_names[i]);
        ASSERT_REJECTED(parse_mqtt(body), "mqtt.tls.ca_file:");
    }
    TEST_ASSERT_TRUE(gw_config_file_name_ok("ca.pem"));
    TEST_ASSERT_TRUE(gw_config_file_name_ok("A-z_0.9"));
    TEST_ASSERT_FALSE(gw_config_file_name_ok(""));
    TEST_ASSERT_FALSE(gw_config_file_name_ok("config.json"));
    ASSERT_REJECTED(parse_mqtt("\"host\":\"b\",\"tls\":{\"enabled\":1}"),
                    "mqtt.tls.enabled: se esperaba true o false");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"b\",\"tls\":{\"verify\":false}"),
                    "mqtt.tls.verify: clave desconocida");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"b\",\"tls\":true"), "mqtt.tls: se esperaba un objeto JSON");
}

/* MQTT-5: client certificate and key (mutual TLS), both or neither. */
static void test_tls_client_certificate(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\",\"tls\":{\"enabled\":true}"), err.msg);
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt.tls.cert_file);
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt.tls.key_file);
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\",\"tls\":{\"enabled\":true,"
                                        "\"cert_file\":\"gw01.crt\",\"key_file\":\"gw01.key\"}"),
                             err.msg);
    TEST_ASSERT_EQUAL_STRING("gw01.crt", cfg.mqtt.tls.cert_file);
    TEST_ASSERT_EQUAL_STRING("gw01.key", cfg.mqtt.tls.key_file);
    ASSERT_REJECTED(parse_mqtt("\"host\":\"b\",\"tls\":{\"enabled\":true,\"cert_file\":\"gw01.crt\"}"),
                    "mqtt.tls.key_file: cert_file y key_file van juntos");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"b\",\"tls\":{\"enabled\":true,\"key_file\":\"gw01.key\"}"),
                    "mqtt.tls.cert_file: cert_file y key_file van juntos");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"b\",\"tls\":{\"cert_file\":\"a/b.crt\","
                               "\"key_file\":\"k\"}"),
                    "mqtt.tls.cert_file: 'a/b.crt' no es un nombre de archivo válido");
}

/* MQTT-6: ordered backup brokers, failover count and return-to-primary policy. */
static void test_backup_brokers(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\""), err.msg);
    TEST_ASSERT_EQUAL(0, cfg.mqtt.backup_count);   /* absent: one broker, as in the MVP */
    TEST_ASSERT_EQUAL(3, cfg.mqtt.failover_after);
    TEST_ASSERT_EQUAL(0, cfg.mqtt.primary_return_s);

    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"a\",\"port\":1884,\"backup_brokers\":["
                                        "{\"host\":\"b\"},{\"host\":\"fd00::2\",\"port\":2883}],"
                                        "\"failover_after\":1,\"primary_return_s\":600"),
                             err.msg);
    TEST_ASSERT_EQUAL(2, cfg.mqtt.backup_count);
    TEST_ASSERT_EQUAL_STRING("b", cfg.mqtt.backup[0].host);
    TEST_ASSERT_EQUAL(1884, cfg.mqtt.backup[0].port);   /* the primary's port */
    TEST_ASSERT_EQUAL_STRING("fd00::2", cfg.mqtt.backup[1].host);
    TEST_ASSERT_EQUAL(2883, cfg.mqtt.backup[1].port);
    TEST_ASSERT_EQUAL(1, cfg.mqtt.failover_after);
    TEST_ASSERT_EQUAL(600, cfg.mqtt.primary_return_s);
    /* with TLS the backups default to 8883 as well */
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"a\",\"tls\":{\"enabled\":true},"
                                        "\"backup_brokers\":[{\"host\":\"b\"}]"),
                             err.msg);
    TEST_ASSERT_EQUAL(8883, cfg.mqtt.backup[0].port);

    ASSERT_REJECTED(parse_mqtt("\"host\":\"a\",\"backup_brokers\":[{\"host\":\"b\"},{\"host\":\"c\"},"
                               "{\"host\":\"d\"},{\"host\":\"e\"}]"),
                    "mqtt.backup_brokers: 4 brokers; el máximo es 3");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"a\",\"backup_brokers\":[{\"port\":1}]"),
                    "mqtt.backup_brokers[0].host: campo obligatorio ausente");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"a\",\"backup_brokers\":[{\"host\":\"b\"},{\"host\":\"\"}]"),
                    "mqtt.backup_brokers[1].host: no puede estar vacío");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"a\",\"backup_brokers\":[{\"host\":\"b\",\"port\":0}]"),
                    "mqtt.backup_brokers[0].port: valor 0 fuera de rango");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"a\",\"backup_brokers\":[{\"host\":\"b\",\"tls\":true}]"),
                    "mqtt.backup_brokers[0].tls: clave desconocida");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"a\",\"backup_brokers\":{\"host\":\"b\"}"),
                    "mqtt.backup_brokers: se esperaba un array");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"a\",\"failover_after\":0"),
                    "mqtt.failover_after: valor 0 fuera de rango [1, 20]");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"a\",\"primary_return_s\":-1"),
                    "mqtt.primary_return_s: valor -1 fuera de rango");
}

/* SPB-14: store & forward switch. */
static void test_store_forward_key(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\""), err.msg);
    TEST_ASSERT_FALSE(cfg.sparkplug.store_forward);   /* absent: coalescing, as in the MVP */
    TEST_ASSERT_TRUE_MESSAGE(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                                   "\"edge_node_id\":\"E\",\"store_forward\":true}}"),
                             err.msg);
    TEST_ASSERT_TRUE(cfg.sparkplug.store_forward);
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\",\"store_forward\":\"yes\"}}"),
                    "sparkplug.store_forward: se esperaba true, false o un objeto");
    TEST_ASSERT_FALSE(cfg.sparkplug.sf_sd);
    /* the object form, with the optional microSD spill (off by default) */
    static const char head[] = "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                               "\"edge_node_id\":\"E\",\"store_forward\":";
    char doc[512];
    snprintf(doc, sizeof(doc), "%s{}}}", head);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_TRUE(cfg.sparkplug.store_forward);
    TEST_ASSERT_FALSE(cfg.sparkplug.sf_sd);
    TEST_ASSERT_EQUAL_UINT32(131072, cfg.sparkplug.sf_sd_max_kb);   /* #124: 128 MiB */
    snprintf(doc, sizeof(doc), "%s{\"sd\":{\"enabled\":true,\"max_kb\":1024}}}}", head);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_TRUE(cfg.sparkplug.sf_sd);
    TEST_ASSERT_EQUAL_UINT32(1024, cfg.sparkplug.sf_sd_max_kb);
    snprintf(doc, sizeof(doc), "%s{\"sd\":{\"enabled\":true}}}}", head);   /* #124 default */
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL_UINT32(GW_SFSPILL_DEFAULT_KB, cfg.sparkplug.sf_sd_max_kb);
    TEST_ASSERT_EQUAL_UINT32(128u * 1024u, GW_SFSPILL_DEFAULT_KB);
    snprintf(doc, sizeof(doc), "%s{\"enabled\":false,\"sd\":{\"enabled\":true}}}}", head);
    ASSERT_REJECTED(parse(doc), "sparkplug.store_forward.sd.enabled: la tarjeta solo extiende");
    snprintf(doc, sizeof(doc), "%s{\"sd\":{\"max_kb\":8}}}}", head);
    ASSERT_REJECTED(parse(doc), "sparkplug.store_forward.sd.max_kb: valor 8 fuera de rango");
    /* the bounds of gw_sfspill_plan() (#119): 64 KB .. 4 GiB */
    snprintf(doc, sizeof(doc), "%s{\"sd\":{\"max_kb\":64}}}}", head);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL_UINT32(GW_SFSPILL_MIN_KB, cfg.sparkplug.sf_sd_max_kb);
    snprintf(doc, sizeof(doc), "%s{\"sd\":{\"max_kb\":4194304}}}}", head);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL_UINT32(GW_SFSPILL_MAX_KB, cfg.sparkplug.sf_sd_max_kb);
    snprintf(doc, sizeof(doc), "%s{\"sd\":{\"max_kb\":4194305}}}}", head);
    ASSERT_REJECTED(parse(doc), "sparkplug.store_forward.sd.max_kb: valor 4194305 fuera de rango");
    snprintf(doc, sizeof(doc), "%s{\"sd\":{\"pins\":1}}}}", head);
    ASSERT_REJECTED(parse(doc), "sparkplug.store_forward.sd.pins: clave desconocida");
    snprintf(doc, sizeof(doc), "%s{\"sd\":true}}}", head);
    ASSERT_REJECTED(parse(doc), "sparkplug.store_forward.sd: se esperaba un objeto JSON");
}

/* MQTT-8: protocol version. */
static void test_mqtt_protocol(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\""), err.msg);
    TEST_ASSERT_EQUAL(GW_MQTT_V311, cfg.mqtt.protocol);   /* absent: 3.1.1, as in the MVP */
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\",\"protocol\":\"5\""), err.msg);
    TEST_ASSERT_EQUAL(GW_MQTT_V5, cfg.mqtt.protocol);
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\",\"protocol\":\"3.1.1\""), err.msg);
    TEST_ASSERT_EQUAL(GW_MQTT_V311, cfg.mqtt.protocol);
    ASSERT_REJECTED(parse_mqtt("\"host\":\"b\",\"protocol\":5"),
                    "mqtt.protocol: valor no válido (se esperaba 3.1.1|5)");
    ASSERT_REJECTED(parse_mqtt("\"host\":\"b\",\"protocol\":\"3.1\""),
                    "mqtt.protocol: valor no válido (se esperaba 3.1.1|5)");
}

/* Issue #64: HTTPS keys of the web section. */
static void test_web_https(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse_mqtt("\"host\":\"b\""), err.msg);
    TEST_ASSERT_TRUE(cfg.web.https);            /* on by default */
    TEST_ASSERT_EQUAL(443, cfg.web.https_port);
    TEST_ASSERT_EQUAL(80, cfg.web.port);
    TEST_ASSERT_TRUE_MESSAGE(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                                   "\"edge_node_id\":\"E\"},\"web\":{\"https_port\":8443,"
                                   "\"port\":8080}}"),
                             err.msg);
    TEST_ASSERT_EQUAL(8443, cfg.web.https_port);
    TEST_ASSERT_TRUE_MESSAGE(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                                   "\"edge_node_id\":\"E\"},\"web\":{\"https\":false,"
                                   "\"https_port\":80}}"),
                             err.msg);   /* the HTTPS port does not matter without HTTPS */
    TEST_ASSERT_FALSE(cfg.web.https);
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"web\":{\"https_port\":80}}"),
                    "web.https_port: no puede ser igual a web.port (80)");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"web\":{\"https\":\"on\"}}"),
                    "web.https: se esperaba true o false");
}

/* CFG-3 / CFG-4: optional "web" section. */
static void test_web_section(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                                   "\"edge_node_id\":\"E\"}}"),
                             err.msg);
    TEST_ASSERT_TRUE(cfg.web.enabled);
    TEST_ASSERT_EQUAL(80, cfg.web.port);
    TEST_ASSERT_EQUAL_STRING("admin", cfg.web.username);
    TEST_ASSERT_EQUAL_STRING("", cfg.web.password);
    TEST_ASSERT_EQUAL_STRING("", cfg.web.password_hash);
    TEST_ASSERT_TRUE_MESSAGE(
        parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":"
              "\"E\"},\"web\":{\"enabled\":false,\"port\":8080,\"username\":\"ops\","
              "\"password\":\"12345678\"}}"),
        err.msg);
    TEST_ASSERT_FALSE(cfg.web.enabled);
    TEST_ASSERT_EQUAL(8080, cfg.web.port);
    TEST_ASSERT_EQUAL_STRING("ops", cfg.web.username);
    TEST_ASSERT_EQUAL_STRING("12345678", cfg.web.password);
    static const char *const bad[][2] = {
        {"{\"port\":0}", "web.port"},
        {"{\"username\":\"\"}", "web.username: no puede estar vacío"},
        {"{\"username\":\"a:b\"}", "web.username: solo caracteres ASCII visibles"},
        {"{\"username\":\"a b\"}", "web.username"},
        {"{\"password\":\"1234567\"}", "web.password: se esperaban al menos 8"},
        {"{\"user\":\"x\"}", "web.user: clave desconocida"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char doc[256];
        snprintf(doc, sizeof(doc),
                 "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":"
                 "\"E\"},\"web\":%s}",
                 bad[i][0]);
        ASSERT_REJECTED(parse(doc), bad[i][1]);
    }
}

/* NET-6: host name, explicit or derived from edge_node_id; mDNS on by default. */
static void test_hostname(void)
{
    TEST_ASSERT_TRUE_MESSAGE(parse_network("{\"dhcp\":true}"), err.msg);
    TEST_ASSERT_EQUAL_STRING("e", cfg.network.hostname);   /* edge_node_id "E" */
    TEST_ASSERT_TRUE(cfg.network.mdns);
    TEST_ASSERT_TRUE_MESSAGE(parse_network("{\"hostname\":\"Plant-GW-01\",\"mdns\":false}"),
                             err.msg);
    TEST_ASSERT_EQUAL_STRING("Plant-GW-01", cfg.network.hostname);
    TEST_ASSERT_FALSE(cfg.network.mdns);
    ASSERT_REJECTED(parse_network("{\"hostname\":\"gw.local\"}"), "network.hostname");
    ASSERT_REJECTED(parse_network("{\"hostname\":\"-gw\"}"), "network.hostname");
    ASSERT_REJECTED(parse_network("{\"hostname\":\"gw-\"}"), "network.hostname");
    ASSERT_REJECTED(parse_network("{\"hostname\":\"g w\"}"), "network.hostname");
    char h[GW_HOSTNAME_SIZE];
    gw_hostname_from("GW_01 Línea/2", h, sizeof(h));
    TEST_ASSERT_EQUAL_STRING("gw-01-l-nea-2", h);
    gw_hostname_from("__", h, sizeof(h));
    TEST_ASSERT_EQUAL_STRING("gateway", h);
    char long_id[100];
    memset(long_id, 'a', 99);
    long_id[99] = '\0';
    gw_hostname_from(long_id, h, sizeof(h));
    TEST_ASSERT_EQUAL(63, strlen(h));
}

/* CFG-4: the REST API returns the error's JSON path, line and column as separate fields. */
static void test_error_fields(void)
{
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\",\"port\":0},\"sparkplug\":{\"group_id\":"
                          "\"G\",\"edge_node_id\":\"E\"}}"),
                    "mqtt.port");
    TEST_ASSERT_EQUAL_STRING("mqtt.port", err.path);
    TEST_ASSERT_EQUAL(0, err.line);
    ASSERT_REJECTED(parse("{\n  \"mqtt\": {\"host\": \"b\"}\n  \"sparkplug\": {}\n}"), "línea 3");
    TEST_ASSERT_EQUAL_STRING("", err.path);
    TEST_ASSERT_EQUAL(3, err.line);
    TEST_ASSERT_EQUAL(3, err.column);
    ASSERT_REJECTED(parse("[]"), "$:");
    TEST_ASSERT_EQUAL_STRING("$", err.path);
    /* nesting is bounded before cJSON recurses (stack of the HTTP and console tasks) */
    static char deep[4096];
    size_t n = 0;
    for (int i = 0; i < 2000; i++) {
        deep[n++] = '[';
    }
    deep[n] = '\0';
    ASSERT_REJECTED(parse(deep), "JSON anidado a más de 32 niveles");
    TEST_ASSERT_EQUAL(4, gw_json_depth("{\"a\":[{\"b\":[1]}],\"s\":\"[[[[[\\\"[[\"}", 40));
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
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int128\"}"),
                    "bool|uint16|int16|uint32|int32|float32|uint64|int64|float64|string");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":65535,\"type\":\"int32\"}"),
                    "65535");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":1.5,\"type\":\"int16\"}"),
                    "entero");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"scale\":0}"),
                    "scale");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"word_order\":\"middle\"}"),
                    "big|little|ABCD|CDAB|BADC|DCBA");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"adress\":0,\"type\":\"int16\"}"),
                    "devices[0].tags[0].adress: clave desconocida");
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

/* MB-6: 64-bit types, strings, byte orders and register bits. */
static void test_mb6_tags(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        parse_tag("{\"name\":\"a\",\"area\":\"holding\",\"address\":0,\"type\":\"uint64\"},"
                  "{\"name\":\"b\",\"area\":\"input\",\"address\":4,\"type\":\"int64\","
                  "\"word_order\":\"CDAB\",\"deadband\":10},"
                  "{\"name\":\"c\",\"area\":\"input\",\"address\":8,\"type\":\"float64\","
                  "\"word_order\":\"DCBA\",\"scale\":2},"
                  "{\"name\":\"d\",\"area\":\"holding\",\"address\":12,\"type\":\"string\","
                  "\"length\":16,\"word_order\":\"BADC\",\"writable\":true},"
                  "{\"name\":\"e\",\"area\":\"holding\",\"address\":40,\"type\":\"bool\","
                  "\"bit\":15,\"writable\":true},"
                  "{\"name\":\"f\",\"area\":\"input\",\"address\":40,\"type\":\"bool\",\"bit\":0},"
                  "{\"name\":\"g\",\"area\":\"input\",\"address\":41,\"type\":\"int32\","
                  "\"word_order\":\"ABCD\"}"),
        err.msg);
    TEST_ASSERT_EQUAL(GW_TYPE_UINT64, cfg.tags[0].type);
    TEST_ASSERT_EQUAL(4, gw_tag_width(&cfg.tags[0]));
    TEST_ASSERT_EQUAL(GW_NO_BIT, cfg.tags[0].bit);
    TEST_ASSERT_EQUAL(0, cfg.tags[0].length);
    TEST_ASSERT_EQUAL(GW_WORD_LITTLE, cfg.tags[1].word_order);
    TEST_ASSERT_EQUAL(GW_WORD_DCBA, cfg.tags[2].word_order);
    TEST_ASSERT_EQUAL(GW_TYPE_STRING, cfg.tags[3].type);
    TEST_ASSERT_EQUAL(16, cfg.tags[3].length);
    TEST_ASSERT_EQUAL(16, gw_tag_width(&cfg.tags[3]));
    TEST_ASSERT_EQUAL(GW_WORD_BADC, cfg.tags[3].word_order);
    TEST_ASSERT_EQUAL(15, cfg.tags[4].bit);
    TEST_ASSERT_EQUAL(1, gw_tag_width(&cfg.tags[4]));
    TEST_ASSERT_EQUAL(0, cfg.tags[5].bit);
    TEST_ASSERT_EQUAL(GW_WORD_BIG, cfg.tags[6].word_order);

    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"string\"}"),
                    "devices[0].tags[0].length: obligatorio con type=string");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"string\","
                              "\"length\":17}"),
                    "devices[0].tags[0].length: valor 17 fuera de rango [1, 16]");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"int16\","
                              "\"length\":2}"),
                    "length: solo vale con type=string");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"string\","
                              "\"length\":2,\"scale\":2}"),
                    "scale/offset no se admiten en tags string");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"string\","
                              "\"length\":2,\"deadband\":1}"),
                    "deadband: no se admite en tags string");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":65533,\"type\":\"int64\"}"),
                    "65535");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"bool\","
                              "\"bit\":16}"),
                    "devices[0].tags[0].bit: valor 16 fuera de rango [0, 15]");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"coil\",\"address\":0,\"type\":\"bool\","
                              "\"bit\":1}"),
                    "bit: solo vale con type=bool en las áreas holding e input");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"uint16\","
                              "\"bit\":1}"),
                    "bit: solo vale con type=bool");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"bool\","
                              "\"bit\":1,\"word_order\":\"BADC\"}"),
                    "word_order: no se admite en un tag bit");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"bool\","
                              "\"bit\":1,\"writable\":true}"),
                    "writable: solo las áreas coil y holding");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"bool\","
                              "\"bit\":1,\"scale\":2}"),
                    "scale/offset no se admiten en tags bool");
}

/* SPB-16: engineering range of a tag (units already existed). */
static void test_eng_range(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        parse_tag("{\"name\":\"a\",\"area\":\"input\",\"address\":0,\"type\":\"float32\","
                  "\"units\":\"degC\",\"eng_low\":-40,\"eng_high\":125.5},"
                  "{\"name\":\"b\",\"area\":\"input\",\"address\":2,\"type\":\"uint16\","
                  "\"eng_high\":100},"
                  "{\"name\":\"c\",\"area\":\"input\",\"address\":3,\"type\":\"uint16\"}"),
        err.msg);
    TEST_ASSERT_EQUAL_STRING("degC", cfg.tags[0].units);
    TEST_ASSERT_TRUE(cfg.tags[0].has_eng_low);
    TEST_ASSERT_EQUAL_DOUBLE(-40, cfg.tags[0].eng_low);
    TEST_ASSERT_TRUE(cfg.tags[0].has_eng_high);
    TEST_ASSERT_EQUAL_DOUBLE(125.5, cfg.tags[0].eng_high);
    TEST_ASSERT_FALSE(cfg.tags[1].has_eng_low);
    TEST_ASSERT_TRUE(cfg.tags[1].has_eng_high);
    TEST_ASSERT_FALSE(cfg.tags[2].has_eng_low);
    TEST_ASSERT_FALSE(cfg.tags[2].has_eng_high);

    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"eng_low\":10,\"eng_high\":10}"),
                    "devices[0].tags[0].eng_high: debe ser mayor que eng_low (10)");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"eng_low\":\"0\"}"),
                    "devices[0].tags[0].eng_low: se esperaba un número");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"coil\",\"address\":0,\"type\":\"bool\","
                              "\"eng_high\":1}"),
                    "devices[0].tags[0].eng_high: no se admite en tags bool");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"string\","
                              "\"length\":2,\"eng_low\":1}"),
                    "devices[0].tags[0].eng_low: no se admite en tags string");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"eng_high\":1e39}"),
                    "devices[0].tags[0].eng_high: fuera del rango de un Float");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"units\":\"0123456789abcdef\"}"),
                    "devices[0].tags[0].units");
}

/* SPB-19: bytes tags are like strings without the text processing. */
static void test_bytes_tags(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        parse_tag("{\"name\":\"raw\",\"area\":\"holding\",\"address\":0,\"type\":\"bytes\","
                  "\"length\":16,\"writable\":true,\"word_order\":\"BADC\"}"),
        err.msg);
    TEST_ASSERT_EQUAL(GW_TYPE_BYTES, cfg.tags[0].type);
    TEST_ASSERT_EQUAL(16, gw_tag_width(&cfg.tags[0]));
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"bytes\"}"),
                    "devices[0].tags[0].length: obligatorio con type=bytes");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"bytes\","
                              "\"length\":2,\"deadband\":1}"),
                    "deadband: no se admite en tags bytes");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"bytes\","
                              "\"length\":2,\"scale\":2}"),
                    "scale/offset no se admiten en tags bytes");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"coil\",\"address\":0,\"type\":\"bytes\","
                              "\"length\":2}"),
                    "las áreas coil/discrete solo admiten type=bool");
}

/* SPB-18: devices of a type have the same tags; the definitions stay bounded. */
static void test_device_types(void)
{
    static const char ok[] =
        "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"},"
        "\"devices\":["
        "{\"name\":\"A\",\"unit_id\":1,\"type\":\"Meter\",\"tags\":[{\"name\":\"v\",\"area\":\"input\","
        "\"address\":0,\"type\":\"uint16\"}]},"
        "{\"name\":\"B\",\"unit_id\":2,\"type\":\"Meter\",\"tags\":[{\"name\":\"v\",\"area\":\"holding\","
        "\"address\":7,\"type\":\"uint16\",\"units\":\"V\"}]},"
        "{\"name\":\"C\",\"unit_id\":3,\"tags\":[{\"name\":\"v\",\"area\":\"input\","
        "\"address\":0,\"type\":\"int16\"}]}]}";
    TEST_ASSERT_TRUE_MESSAGE(parse(ok), err.msg);   /* area, address and units may differ */
    TEST_ASSERT_EQUAL_STRING("Meter", cfg.devices[1].type);
    TEST_ASSERT_EQUAL_STRING("", cfg.devices[2].type);

    char doc[1024];
    snprintf(doc, sizeof(doc), "%s", ok);
    char *t = strstr(doc, "\"address\":7,\"type\":\"uint16\"");
    memcpy(t + strlen("\"address\":7,\"type\":\""), "uint32", 6);   /* other datatype */
    ASSERT_REJECTED(parse(doc), "devices[1].type: el tipo 'Meter' ya lo define devices[0] (A)");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"},"
                          "\"devices\":[{\"name\":\"A\",\"unit_id\":1,\"type\":\"M/1\",\"tags\":[{\"name\":\"v\","
                          "\"area\":\"input\",\"address\":0,\"type\":\"uint16\"}]}]}"),
                    "devices[0].type: carácter no permitido '/'");

    /* the definitions together: at most GW_MAX_TEMPLATE_TAGS tags */
    static char big[16384];
    size_t n = (size_t)snprintf(big, sizeof(big), "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":"
                                "{\"group_id\":\"G\",\"edge_node_id\":\"E\"},\"devices\":[");
    for (int d = 0; d < 3; d++) {
        n += (size_t)snprintf(big + n, sizeof(big) - n, "%s{\"name\":\"D%d\",\"unit_id\":1,"
                              "\"type\":\"T%d\",\"tags\":[", d ? "," : "", d, d);
        for (int k = 0; k < 30; k++) {
            n += (size_t)snprintf(big + n, sizeof(big) - n, "%s{\"name\":\"t%d\",\"area\":\"input\","
                                  "\"address\":%d,\"type\":\"uint16\"}", k ? "," : "", k, k);
        }
        n += (size_t)snprintf(big + n, sizeof(big) - n, "]}");
    }
    snprintf(big + n, sizeof(big) - n, "]}");
    ASSERT_REJECTED(parse(big), "devices[2].type: los tipos de dispositivo describen más de 64 tags");
}

/* Issue #162: well-formed UTF-8 (RFC 3629) */
static void test_utf8_sequences(void)
{
    TEST_ASSERT_TRUE(gw_utf8_valid(""));
    TEST_ASSERT_TRUE(gw_utf8_valid("Planta Nº1 \xc3\xa9t\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"));
    TEST_ASSERT_TRUE(gw_utf8_valid("\xed\x9f\xbf\xee\x80\x80\xf4\x8f\xbf\xbf"));   /* U+D7FF, U+E000, U+10FFFF */
    static const char *const bad[] = {
        "\x80",                 /* continuation byte alone */
        "a\xc3",                /* truncated */
        "\xc0\xaf",             /* overlong '/' */
        "\xc1\xbf",             /* overlong */
        "\xe0\x80\xaf",         /* overlong */
        "\xf0\x80\x80\xaf",     /* overlong */
        "\xed\xa0\x80",         /* surrogate U+D800 */
        "\xed\xbf\xbf",         /* surrogate U+DFFF */
        "\xf4\x90\x80\x80",     /* U+110000 */
        "\xf5\x80\x80\x80",     /* lead byte F5 */
        "\xff",
        "\xe2\x82",             /* truncated */
        "\xe2\x28\xa1",         /* bad continuation */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(gw_utf8_valid(bad[i]), bad[i]);
    }
    TEST_ASSERT_EQUAL(2, gw_utf8_seq("\xc2\x85", 2));
    TEST_ASSERT_EQUAL_HEX32(0x85, gw_utf8_code("\xc2\x85", 2));
    TEST_ASSERT_EQUAL_HEX32(0x1F600, gw_utf8_code("\xf0\x9f\x98\x80", 4));
    TEST_ASSERT_EQUAL(0, gw_utf8_seq("\xc2\x85", 1));
}

/* Issue #162 (CFG-1): Sparkplug IDs and names: valid UTF-8, no two names that differ only in
 * case (tck-id-case-sensitivity-sparkplug-ids, tck-id-case-sensitivity-metric-names), and no
 * metric that is also a folder of another metric. */
/* #172: sparkplug.device_prefix, a list of location levels before each device name. */
static void test_device_prefix(void)
{
    char doc[1024];
#define PDOC(group, node, prefix, name)                                                           \
    (snprintf(doc, sizeof(doc),                                                                    \
              "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"%s\",\"edge_node_id\":"    \
              "\"%s\",\"device_prefix\":\"%s\"},\"devices\":[{\"name\":\"%s\",\"unit_id\":1,"      \
              "\"tags\":[{\"name\":\"v\",\"area\":\"input\",\"address\":0,\"type\":\"uint16\"}]}]}", \
              group, node, prefix, name), parse(doc))
    char id[GW_TOPIC_SIZE];
    /* absent: the device_id is the name */
    TEST_ASSERT_TRUE_MESSAGE(PDOC("G", "E", "", "meter1"), err.msg);
    TEST_ASSERT_EQUAL_STRING("", cfg.sparkplug.device_prefix);
    TEST_ASSERT_TRUE(gw_config_device_id(&cfg, 0, id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING("meter1", id);
    /* free depth: one level or many */
    TEST_ASSERT_TRUE_MESSAGE(PDOC("G", "E", "acme", "meter1"), err.msg);
    TEST_ASSERT_TRUE(gw_config_device_id(&cfg, 0, id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING("acme:meter1", id);
    TEST_ASSERT_TRUE_MESSAGE(PDOC("G", "E", "acme:berlin:packaging:line3:cell2:station4", "m1"),
                             err.msg);
    TEST_ASSERT_TRUE(gw_config_device_id(&cfg, 0, id, sizeof(id)));
    TEST_ASSERT_EQUAL_STRING("acme:berlin:packaging:line3:cell2:station4:m1", id);
    TEST_ASSERT_FALSE(gw_config_device_id(&cfg, 0, id, 10));   /* does not fit: false */
    /* each level must exist; the characters are a topic ID (#162) */
    ASSERT_REJECTED(PDOC("G", "E", ":acme", "m"), "sparkplug.device_prefix: empty level at byte 0");
    ASSERT_REJECTED(PDOC("G", "E", "acme:", "m"), "sparkplug.device_prefix: empty level at byte 4");
    ASSERT_REJECTED(PDOC("G", "E", "acme::line3", "m"), "sparkplug.device_prefix: empty level");
    ASSERT_REJECTED(PDOC("G", "E", "acme/line3", "m"), "sparkplug.device_prefix: car");
    ASSERT_REJECTED(PDOC("G", "E", "acme+", "m"), "sparkplug.device_prefix: car");
    ASSERT_REJECTED(PDOC("G", "E", "acme\xc3", "m"), "sparkplug.device_prefix: not valid UTF-8");
    /* 96 bytes at most */
    char longp[GW_PREFIX_SIZE + 1];
    memset(longp, 'a', GW_PREFIX_SIZE);
    longp[GW_PREFIX_SIZE] = '\0';
    ASSERT_REJECTED(PDOC("G", "E", longp, "m"), "sparkplug.device_prefix: texto demasiado largo");
    /* the full device_id: the longest topic (DBIRTH) fits GW_TOPIC_SIZE - 1 = 159 bytes. With
     * a group and a node of 32 bytes, 159 - 8 - 32 - 8 - 32 - 1 = 78 bytes are left */
    const char *g32 = "gggggggggggggggggggggggggggggggg", *e32 = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    char p60[61], p59[60];
    memset(p60, 'p', 60);
    p60[60] = '\0';
    memset(p59, 'p', 59);
    p59[59] = '\0';
    p59[10] = p60[10] = ':';
    TEST_ASSERT_TRUE_MESSAGE(PDOC(g32, e32, p59, "abcdefghijklmnopqr"), err.msg);   /* 78 */
    TEST_ASSERT_TRUE(gw_config_device_id(&cfg, 0, id, sizeof(id)));
    TEST_ASSERT_EQUAL_size_t(78, strlen(id));
    ASSERT_REJECTED(PDOC(g32, e32, p60, "abcdefghijklmnopqr"),
                    "devices[0].name: the device_id (sparkplug.device_prefix, ':' and the name) "
                    "has 79 bytes; with this group_id and edge_node_id the maximum is 78");
    /* shorter group and node IDs leave more room: the check uses the real lengths */
    TEST_ASSERT_TRUE_MESSAGE(PDOC("G", "E", p60, "abcdefghijklmnopqrstuvwxyz0123456789ABC"),
                             err.msg);
#undef PDOC
}

/* #169: the findings of gw_config_lint, kept for the asserts. */
#define LINT_KEEP 16
static struct {
    unsigned calls, errors, stop_after;
    char path[LINT_KEEP][GW_CFG_PATH_SIZE];
    char msg[LINT_KEEP][GW_CFG_ERR_SIZE];
} lr;

static bool lint_cb(void *ctx, gw_lint_level_t level, const char *path, const char *msg)
{
    TEST_ASSERT_EQUAL_PTR(&lr, ctx);
    if (lr.calls < LINT_KEEP) {
        snprintf(lr.path[lr.calls], sizeof(lr.path[0]), "%s", path);
        snprintf(lr.msg[lr.calls], sizeof(lr.msg[0]), "%s", msg);
    }
    lr.calls++;
    lr.errors += level == GW_LINT_ERROR;
    return lr.stop_after == 0 || lr.calls < lr.stop_after;
}

static size_t lint(void)
{
    memset(&lr.calls, 0, sizeof(lr.calls));
    lr.errors = 0;
    memset(lr.path, 0, sizeof(lr.path));
    memset(lr.msg, 0, sizeof(lr.msg));
    return gw_config_lint(&cfg, lint_cb, &lr);
}

/* The index of the finding with @p path whose message has @p text, or -1. */
static int lint_find(const char *path, const char *text)
{
    for (unsigned i = 0; i < lr.calls && i < LINT_KEEP; i++) {
        if (strcmp(lr.path[i], path) == 0 && strstr(lr.msg[i], text) != NULL) {
            return (int)i;
        }
    }
    return -1;
}

#define ASSERT_LINT(path, text) \
    TEST_ASSERT_TRUE_MESSAGE(lint_find(path, text) >= 0, path ": " text)

#define LINT_DOC(naming, edge, prefix, name, tags)                                                \
    "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"" edge "\"," \
    "\"device_prefix\":\"" prefix "\"" naming "},\"devices\":[{\"name\":\"" name "\","             \
    "\"unit_id\":1,\"tags\":[" tags "]}]}"
#define LTAG(name) "{\"name\":\"" name "\",\"area\":\"input\",\"address\":0,\"type\":\"uint16\"}"

/* #169: the name rules for UNS hosts: warnings with JSON paths, never in the way of a valid
 * configuration. */
static void test_lint_name_rules(void)
{
    /* clean names: no finding (the SPEC example too) */
    size_t len;
    const char *json = support_read_file(GW_SPEC_EXAMPLE, &len);
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(json, len, &cfg, &err), err.msg);
    TEST_ASSERT_EQUAL_size_t(0, lint());
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC("", "acme:berlin:gw-01", "acme:berlin:pack_2",
                                            "line3:meter_1", LTAG("L1/Current") "," LTAG("x.y-z_1"))),
                             err.msg);
    TEST_ASSERT_EQUAL_size_t(0, lint());
    TEST_ASSERT_EQUAL_size_t(0, gw_config_lint(&cfg, NULL, NULL));

    /* location IDs: each level matches [A-Za-z0-9][A-Za-z0-9_-]* */
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC("", "acme: berlin", "acme:_raw", "Meter.1",
                                            LTAG("v"))), err.msg);
    TEST_ASSERT_EQUAL_size_t(3, lint());
    TEST_ASSERT_EQUAL(0, lr.errors);
    ASSERT_LINT("sparkplug.edge_node_id", "level 2 ' berlin' has a space: UMH changes it to '_'");
    ASSERT_LINT("sparkplug.device_prefix", "level 2 '_raw' starts with '_': UMH reads the level "
                                           "as the data contract");
    ASSERT_LINT("devices[0].name", "level 1 'Meter.1' has '.': UMH reads it as one more level");
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC("", "a::b", "", "M\xc3\xa9ter", LTAG("v"))), err.msg);
    TEST_ASSERT_EQUAL_size_t(2, lint());
    ASSERT_LINT("sparkplug.edge_node_id", "level 2 '' is empty");
    ASSERT_LINT("devices[0].name", "has a character that is not ASCII");
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC("", ":gw", "", "-m:x$", LTAG("v"))), err.msg);
    TEST_ASSERT_EQUAL_size_t(3, lint());
    ASSERT_LINT("sparkplug.edge_node_id", "level 1 '' is empty");
    ASSERT_LINT("devices[0].name", "level 1 '-m' does not start with a letter or a digit ('-')");
    ASSERT_LINT("devices[0].name", "level 2 'x$' has a character that UMH changes to '_' ('$')");

    /* metric names: each segment between '/' matches [A-Za-z0-9][A-Za-z0-9._-]* */
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC("", "gw", "", "m",
                                            LTAG("L1 Current") "," LTAG("L1/_x") "," LTAG("a:b") ","
                                            LTAG("x#y") "," LTAG("/lead") "," LTAG("p/\xc2\xb5s"))),
                             err.msg);
    TEST_ASSERT_EQUAL_size_t(6, lint());
    ASSERT_LINT("devices[0].tags[0].name", "segment 1 'L1 Current' of the metric name has a space");
    ASSERT_LINT("devices[0].tags[1].name", "segment 2 '_x' of the metric name starts with '_'");
    ASSERT_LINT("devices[0].tags[2].name", "has ':': UMH splits a metric name at the last ':'");
    ASSERT_LINT("devices[0].tags[3].name", "UMH changes to '_' ('#')");
    ASSERT_LINT("devices[0].tags[4].name", "segment 1 '' of the metric name is empty");
    ASSERT_LINT("devices[0].tags[5].name", "segment 2");

    /* other settings that UMH cannot use */
    TEST_ASSERT_TRUE_MESSAGE(parse(
        "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"gw\","
        "\"primary_host_id\":\"SCADA\"},\"devices\":[{\"name\":\"m\",\"type\":\"Pump\","
        "\"unit_id\":1,\"tags\":[{\"name\":\"b\",\"area\":\"input\",\"address\":0,"
        "\"type\":\"bytes\",\"length\":2}]}]}"), err.msg);
    TEST_ASSERT_EQUAL_size_t(3, lint());
    ASSERT_LINT("sparkplug.primary_host_id", "UMH Core cannot be the primary host");
    ASSERT_LINT("devices[0].type", "UMH Core gives no value for a template instance");
    ASSERT_LINT("devices[0].tags[0].type", "UMH Core gives no value for a Bytes metric");

    /* a callback that returns false gets no more calls; the count includes all */
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC("", "a b", "c d", "e f", LTAG("g h"))), err.msg);
    lr.stop_after = 2;
    TEST_ASSERT_EQUAL_size_t(4, lint());
    TEST_ASSERT_EQUAL(2, lr.calls);
    lr.stop_after = 0;
}

/* #169: the UMH preset (sparkplug.naming "umh") refuses the name findings; the other findings
 * stay warnings. */
static void test_lint_umh_preset(void)
{
#define UMH ",\"naming\":\"umh\""
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC(UMH, "acme:gw01", "acme:line3", "meter1",
                                            LTAG("L1/Current"))), err.msg);
    TEST_ASSERT_EQUAL(GW_NAMING_UMH, cfg.sparkplug.naming);
    TEST_ASSERT_EQUAL_size_t(0, lint());
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC(",\"naming\":\"sparkplug\"", "gw", "", "m", LTAG("v"))),
                             err.msg);
    TEST_ASSERT_EQUAL(GW_NAMING_SPARKPLUG, cfg.sparkplug.naming);
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC("", "gw", "", "m", LTAG("v"))), err.msg);
    TEST_ASSERT_EQUAL(GW_NAMING_SPARKPLUG, cfg.sparkplug.naming);   /* the default */
    ASSERT_REJECTED(parse(LINT_DOC(",\"naming\":\"UMH\"", "gw", "", "m", LTAG("v"))),
                    "sparkplug.naming: valor no válido (se esperaba sparkplug|umh)");

    /* a name finding is the error of the validation, with its JSON path */
    ASSERT_REJECTED(parse(LINT_DOC(UMH, "acme: gw01", "", "m", LTAG("v"))),
                    "sparkplug.edge_node_id: level 2 ' gw01' has a space: UMH changes it to '_' "
                    "(sparkplug.naming \"umh\")");
    TEST_ASSERT_EQUAL_STRING("sparkplug.edge_node_id", err.path);
    ASSERT_REJECTED(parse(LINT_DOC(UMH, "gw", "acme:_x", "m", LTAG("v"))),
                    "sparkplug.device_prefix: level 2 '_x' starts with '_'");
    ASSERT_REJECTED(parse(LINT_DOC(UMH, "gw", "", "m", LTAG("v") "," LTAG("a b"))),
                    "devices[0].tags[1].name: segment 1 'a b'");
    TEST_ASSERT_EQUAL_STRING("devices[0].tags[1].name", err.path);
    /* the same names without the preset: valid, with warnings */
    TEST_ASSERT_TRUE_MESSAGE(parse(LINT_DOC("", "gw", "", "m", LTAG("v") "," LTAG("a b"))),
                             err.msg);
    TEST_ASSERT_EQUAL_size_t(1, lint());
    TEST_ASSERT_EQUAL(0, lr.errors);

    /* #173: Devices/<name>/Online, each ':' as '/': "a" and "a:online:x" would make a metric
     * also a folder (without case, as issue #162); another level is good */
#define DEV2(a, b) "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":" \
                   "\"gw\"" UMH "},\"devices\":[{\"name\":\"" a "\",\"unit_id\":1,\"tags\":["     \
                   LTAG("v") "]},{\"name\":\"" b "\",\"unit_id\":2,\"tags\":[" LTAG("v") "]}]}"
    ASSERT_REJECTED(parse(DEV2("line3", "line3:online:x")),
                    "devices[1].name: the node metric Devices/line3/Online of devices[0] would "
                    "also be a folder");
    ASSERT_REJECTED(parse(DEV2("Line3:Online", "line3")), "devices[0].name: the node metric "
                                                          "Devices/line3/Online of devices[1]");
    TEST_ASSERT_TRUE_MESSAGE(parse(DEV2("line3", "line3:onlinex")), err.msg);
    TEST_ASSERT_TRUE_MESSAGE(parse(DEV2("line3", "line3:meter1")), err.msg);
#undef DEV2

    /* not name findings: warnings, also with the preset */
    TEST_ASSERT_TRUE_MESSAGE(parse(
        "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"gw\","
        "\"primary_host_id\":\"SCADA\",\"naming\":\"umh\"},\"devices\":[{\"name\":\"m\","
        "\"type\":\"Pump\",\"unit_id\":1,\"tags\":[{\"name\":\"b\",\"area\":\"input\","
        "\"address\":0,\"type\":\"bytes\",\"length\":2}]}]}"), err.msg);
    TEST_ASSERT_EQUAL_size_t(2, lint());   /* #174: no type warning: plain metrics */
    TEST_ASSERT_EQUAL(0, lr.errors);
    TEST_ASSERT_EQUAL(-1, lint_find("devices[0].type", ""));
#undef UMH
}

/* #172: the mDNS instance name is one DNS label of 63 bytes at most. */
static void test_mdns_instance(void)
{
    char out[GW_MDNS_INSTANCE_SIZE];
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.sparkplug.group_id, "Plant1");
    strcpy(cfg.sparkplug.edge_node_id, "GW01");
    gw_config_mdns_instance(&cfg, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Plant1/GW01", out);
    memset(cfg.sparkplug.group_id, 'g', GW_ID_SIZE - 1);
    memset(cfg.sparkplug.edge_node_id, 'e', GW_ID_SIZE - 1);
    gw_config_mdns_instance(&cfg, out, sizeof(out));   /* 32 + 1 + 32 = 65 bytes */
    TEST_ASSERT_EQUAL_size_t(63, strlen(out));
    TEST_ASSERT_EQUAL_CHAR('/', out[32]);
    /* never cut inside a UTF-8 character: a 3-byte character at bytes 62..64 goes away */
    memset(cfg.sparkplug.edge_node_id, 0, GW_ID_SIZE);
    memset(cfg.sparkplug.edge_node_id, 'e', 29);
    memcpy(cfg.sparkplug.edge_node_id + 29, "\xe2\x82\xac", 3);
    gw_config_mdns_instance(&cfg, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(62, strlen(out));
    TEST_ASSERT_TRUE(gw_utf8_valid(out));
}

static void test_sparkplug_names(void)
{
    char doc[1024];
#define SPB_DOC(group, node)                                                                      \
    (snprintf(doc, sizeof(doc),                                                                    \
              "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"%s\",\"edge_node_id\":"    \
              "\"%s\"}}", group, node), parse(doc))
    TEST_ASSERT_TRUE_MESSAGE(SPB_DOC("Planta \xc3\xa9t\xc3\xa9", "GW\xe2\x82\xac"), err.msg);
    ASSERT_REJECTED(SPB_DOC("G\xc3", "E"), "sparkplug.group_id: not valid UTF-8 (byte 0xc3 at offset 1)");
    ASSERT_REJECTED(SPB_DOC("G", "\xc0\xaf" "E"), "sparkplug.edge_node_id: not valid UTF-8");
    ASSERT_REJECTED(SPB_DOC("G", "E\xed\xa0\x80"), "sparkplug.edge_node_id: not valid UTF-8");
    ASSERT_REJECTED(SPB_DOC("G\xc2\x85", "E"), "sparkplug.group_id: control character U+0085");
    snprintf(doc, sizeof(doc), "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
             "\"edge_node_id\":\"E\",\"primary_host_id\":\"H\xff\"}}");
    ASSERT_REJECTED(parse(doc), "sparkplug.primary_host_id: not valid UTF-8");
#undef SPB_DOC

    /* device IDs */
#define DEV(name, type) "{\"name\":\"" name "\",\"unit_id\":1," type "\"tags\":[{\"name\":\"v\"," \
                        "\"area\":\"input\",\"address\":0,\"type\":\"uint16\"}]}"
#define DEVS(...) "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":" \
                  "\"E\"},\"devices\":[" __VA_ARGS__ "]}"
    TEST_ASSERT_TRUE_MESSAGE(parse(DEVS(DEV("Meter1", "") "," DEV("Meter2", ""))), err.msg);
    ASSERT_REJECTED(parse(DEVS(DEV("Meter1", "") "," DEV("meter1", ""))),
                    "devices[1].name: 'meter1' differs only in case from devices[0] 'Meter1'");
    ASSERT_REJECTED(parse(DEVS(DEV("M\xe9ter", ""))), "devices[0].name: not valid UTF-8 (byte 0xe9");
    /* device types (template definitions in the NBIRTH, next to the node metrics) */
    TEST_ASSERT_TRUE_MESSAGE(parse(DEVS(DEV("A", "\"type\":\"Meter\",") "," DEV("B", "\"type\":\"Meter\","))),
                             err.msg);
    ASSERT_REJECTED(parse(DEVS(DEV("A", "\"type\":\"Meter\",") "," DEV("B", "\"type\":\"METER\","))),
                    "devices[1].type: 'METER' differs only in case from the type 'Meter' of devices[0]");
    ASSERT_REJECTED(parse(DEVS(DEV("A", "\"type\":\"bdSeq\","))),
                    "devices[0].type: 'bdSeq' is the name or a folder of the node metric 'bdSeq'");
    ASSERT_REJECTED(parse(DEVS(DEV("A", "\"type\":\"node info\","))),
                    "the node metric 'Node Info'");
    ASSERT_REJECTED(parse(DEVS(DEV("A", "\"type\":\"DI\","))), "the node metric 'DI'");
    ASSERT_REJECTED(parse(DEVS(DEV("A", "\"type\":\"T\xf8\","))), "devices[0].type: not valid UTF-8");
#undef DEV
#undef DEVS

    /* metric names: UTF-8, case, folders; also the engUnit property */
    TEST_ASSERT_TRUE_MESSAGE(
        parse_tag("{\"name\":\"Temp \xc2\xb0" "C\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                  "\"units\":\"\xc2\xb0" "C\"},"
                  "{\"name\":\"A/B\",\"area\":\"input\",\"address\":1,\"type\":\"int16\"},"
                  "{\"name\":\"A/C\",\"area\":\"input\",\"address\":2,\"type\":\"int16\"},"
                  "{\"name\":\"AB\",\"area\":\"input\",\"address\":3,\"type\":\"int16\"},"
                  "{\"name\":\"A/BC\",\"area\":\"input\",\"address\":4,\"type\":\"int16\"}"),
        err.msg);
    ASSERT_REJECTED(parse_tag("{\"name\":\"T\xb0\",\"area\":\"input\",\"address\":0,\"type\":\"int16\"}"),
                    "devices[0].tags[0].name: not valid UTF-8 (byte 0xb0 at offset 1)");
    ASSERT_REJECTED(parse_tag("{\"name\":\"T\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"units\":\"\xb0" "C\"}"),
                    "devices[0].tags[0].units: not valid UTF-8");
    ASSERT_REJECTED(parse_tag("{\"name\":\"Temp\",\"area\":\"input\",\"address\":0,\"type\":\"int16\"},"
                              "{\"name\":\"TEMP\",\"area\":\"input\",\"address\":1,\"type\":\"int16\"}"),
                    "devices[0].tags[1].name: 'TEMP' differs only in case from tags[0] 'Temp'");
    ASSERT_REJECTED(parse_tag("{\"name\":\"A\",\"area\":\"input\",\"address\":0,\"type\":\"int16\"},"
                              "{\"name\":\"A/B\",\"area\":\"input\",\"address\":1,\"type\":\"int16\"}"),
                    "devices[0].tags[1].name: 'A/B' and tags[0] 'A': a metric name cannot also be "
                    "a folder of another metric");
    ASSERT_REJECTED(parse_tag("{\"name\":\"x/A/B\",\"area\":\"input\",\"address\":0,\"type\":\"int16\"},"
                              "{\"name\":\"x/a\",\"area\":\"input\",\"address\":1,\"type\":\"int16\"}"),
                    "devices[0].tags[1].name: 'x/a' and tags[0] 'x/A/B'");
    /* names in two devices do not conflict: each device has its own metrics (its DBIRTH) */
    TEST_ASSERT_TRUE_MESSAGE(
        parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"},"
              "\"devices\":[{\"name\":\"A\",\"unit_id\":1,\"tags\":[{\"name\":\"v\",\"area\":\"input\","
              "\"address\":0,\"type\":\"uint16\"}]},{\"name\":\"B\",\"unit_id\":2,\"tags\":[{\"name\":"
              "\"v/x\",\"area\":\"input\",\"address\":1,\"type\":\"uint16\"}]},{\"name\":\"C\","
              "\"unit_id\":3,\"tags\":[{\"name\":\"V\",\"area\":\"input\",\"address\":1,\"type\":"
              "\"uint16\"}]}]}"),
        err.msg);
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"},"
                          "\"devices\":[{\"name\":\"B\",\"unit_id\":2,\"tags\":[{\"name\":"
                          "\"V\",\"area\":\"input\",\"address\":0,\"type\":\"uint16\"},{\"name\":\"v/x\",\"area\":"
                          "\"input\",\"address\":1,\"type\":\"uint16\"}]}]}"),
                    "devices[0].tags[1].name: 'v/x' and tags[0] 'V'");
}

/* Issue #38: block reads per device. */
static void test_device_block_reads(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"},"
              "\"devices\":[{\"name\":\"A\",\"unit_id\":1,\"max_gap\":10,\"tags\":[{\"name\":\"t\","
              "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\"}]},"
              "{\"name\":\"B\",\"unit_id\":2,\"block_reads\":false,\"tags\":[{\"name\":\"t\","
              "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\"}]},"
              "{\"name\":\"C\",\"unit_id\":3,\"tags\":[{\"name\":\"t\","
              "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\"}]}]}"),
        err.msg);
    TEST_ASSERT_EQUAL(GW_CFG_BUILD_DEFAULT, cfg.devices[0].block_reads);
    TEST_ASSERT_EQUAL(10, cfg.devices[0].max_gap);
    TEST_ASSERT_EQUAL(0, cfg.devices[1].block_reads);
    TEST_ASSERT_EQUAL(GW_CFG_BUILD_DEFAULT, cfg.devices[1].max_gap);
    TEST_ASSERT_EQUAL(GW_CFG_BUILD_DEFAULT, cfg.devices[2].block_reads);
    TEST_ASSERT_EQUAL(GW_CFG_BUILD_DEFAULT, cfg.devices[2].max_gap);
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"devices\":[{\"name\":\"A\",\"unit_id\":1,"
                          "\"block_reads\":false,\"max_gap\":3,\"tags\":[{\"name\":\"t\","
                          "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\"}]}]}"),
                    "devices[0].max_gap: no tiene efecto con block_reads=false");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"devices\":[{\"name\":\"A\",\"unit_id\":1,"
                          "\"max_gap\":2000,\"tags\":[{\"name\":\"t\","
                          "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\"}]}]}"),
                    "devices[0].max_gap: valor 2000 fuera de rango [0, 1999]");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"devices\":[{\"name\":\"A\",\"unit_id\":1,"
                          "\"block_reads\":1,\"tags\":[{\"name\":\"t\","
                          "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\"}]}]}"),
                    "devices[0].block_reads: se esperaba true o false");
}

/* MB-12: scan classes. */
static void test_scan_classes(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"},"
              "\"devices\":[{\"name\":\"A\",\"unit_id\":1,\"poll_ms\":500,\"tags\":[{\"name\":\"t\","
              "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\",\"scan\":\"slow\"},"
              "{\"name\":\"u\",\"area\":\"holding\",\"address\":1,\"type\":\"uint16\"}]},"
              "{\"name\":\"B\",\"unit_id\":2,\"poll_ms\":1000000,\"slow_poll_ms\":3600000,"
              "\"tags\":[{\"name\":\"t\",\"area\":\"holding\",\"address\":0,\"type\":\"uint16\","
              "\"scan\":\"fast\"}]},"
              "{\"name\":\"C\",\"unit_id\":3,\"poll_ms\":1000000,\"tags\":[{\"name\":\"t\","
              "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\"}]}]}"),
        err.msg);
    TEST_ASSERT_TRUE(cfg.tags[0].slow);
    TEST_ASSERT_FALSE(cfg.tags[1].slow);
    TEST_ASSERT_FALSE(cfg.tags[2].slow);
    TEST_ASSERT_EQUAL_UINT32(5000, cfg.devices[0].slow_poll_ms);     /* default 10 x poll_ms */
    TEST_ASSERT_EQUAL_UINT32(3600000, cfg.devices[1].slow_poll_ms);
    TEST_ASSERT_EQUAL_UINT32(3600000, cfg.devices[2].slow_poll_ms);  /* default capped */
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"devices\":[{\"name\":\"A\",\"unit_id\":1,"
                          "\"poll_ms\":1000,\"slow_poll_ms\":999,\"tags\":[{\"name\":\"t\","
                          "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\"}]}]}"),
                    "devices[0].slow_poll_ms: no puede ser menor que poll_ms (1000)");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"scan\":\"medium\"}"),
                    "devices[0].tags[0].scan: valor no válido (se esperaba fast|slow)");
}

/* MB-15: Modbus TCP gateway section. */
static void test_modbus_tcp_section(void)
{
    const char *base = "\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"}";
    char doc[512];
    snprintf(doc, sizeof(doc), "{%s}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_FALSE(cfg.modbus_tcp.enabled);               /* off unless asked for */
    TEST_ASSERT_EQUAL(502, cfg.modbus_tcp.port);
    TEST_ASSERT_EQUAL(60, cfg.modbus_tcp.idle_timeout_s);
    TEST_ASSERT_EQUAL(0, cfg.modbus_tcp.client_count);
    snprintf(doc, sizeof(doc),
             "{%s,\"modbus_tcp\":{\"enabled\":true,\"port\":1502,\"idle_timeout_s\":0,"
             "\"allowed_clients\":[\"10.0.2.2\",\"192.168.7.0/24\",\"0.0.0.0/0\"]}}",
             base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_TRUE(cfg.modbus_tcp.enabled);
    TEST_ASSERT_EQUAL(1502, cfg.modbus_tcp.port);
    TEST_ASSERT_EQUAL(0, cfg.modbus_tcp.idle_timeout_s);
    TEST_ASSERT_EQUAL(3, cfg.modbus_tcp.client_count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){10, 0, 2, 2}), cfg.modbus_tcp.clients[0].ip, 4);
    TEST_ASSERT_EQUAL(32, cfg.modbus_tcp.clients[0].prefix);
    TEST_ASSERT_EQUAL(24, cfg.modbus_tcp.clients[1].prefix);
    TEST_ASSERT_EQUAL(0, cfg.modbus_tcp.clients[2].prefix);
    snprintf(doc, sizeof(doc), "{%s,\"modbus_tcp\":{\"allowed_clients\":[\"10.0.0.0/33\"]}}", base);
    ASSERT_REJECTED(parse(doc), "modbus_tcp.allowed_clients[0]: prefijo no válido");
    snprintf(doc, sizeof(doc), "{%s,\"modbus_tcp\":{\"allowed_clients\":[\"x\",1]}}", base);
    ASSERT_REJECTED(parse(doc), "modbus_tcp.allowed_clients[0]: 'x' no es una dirección IPv4");
    snprintf(doc, sizeof(doc), "{%s,\"modbus_tcp\":{\"allowed_clients\":[\"1.2.3.4\",1]}}", base);
    ASSERT_REJECTED(parse(doc), "modbus_tcp.allowed_clients[1]: se esperaba un texto");
    snprintf(doc, sizeof(doc), "{%s,\"modbus_tcp\":{\"allowed_clients\":\"1.2.3.4\"}}", base);
    ASSERT_REJECTED(parse(doc), "modbus_tcp.allowed_clients: se esperaba un array");
    snprintf(doc, sizeof(doc),
             "{%s,\"modbus_tcp\":{\"allowed_clients\":[\"1.1.1.1\",\"1.1.1.2\",\"1.1.1.3\","
             "\"1.1.1.4\",\"1.1.1.5\",\"1.1.1.6\",\"1.1.1.7\",\"1.1.1.8\",\"1.1.1.9\"]}}", base);
    ASSERT_REJECTED(parse(doc), "9 entradas; el máximo es 8");
    snprintf(doc, sizeof(doc), "{%s,\"modbus_tcp\":{\"port\":0}}", base);
    ASSERT_REJECTED(parse(doc), "modbus_tcp.port: valor 0 fuera de rango");
    snprintf(doc, sizeof(doc), "{%s,\"modbus_tcp\":{\"enable\":true}}", base);
    ASSERT_REJECTED(parse(doc), "modbus_tcp.enable: clave desconocida");
}

/* IO-3 (and the other optional local_io keys): defaults keep the MVP behaviour. */
static void test_local_io_section(void)
{
    const char *base = "\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"}";
    char doc[768];
    snprintf(doc, sizeof(doc), "{%s}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL(0, cfg.local_io.failsafe_s);        /* no reaction to an MQTT loss */
    TEST_ASSERT_EQUAL(600, cfg.local_io.counter_save_s);  /* IO-4 */
    for (int i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        TEST_ASSERT_EQUAL(GW_DO_SAFE_OFF, cfg.local_io.do_failsafe[i]);
    }
    snprintf(doc, sizeof(doc),
             "{%s,\"local_io\":{\"di_debounce_ms\":10,\"failsafe_s\":30,"
             "\"do_failsafe\":[\"hold\",\"on\",\"off\",\"hold\"]}}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL(30, cfg.local_io.failsafe_s);
    TEST_ASSERT_EQUAL(GW_DO_SAFE_HOLD, cfg.local_io.do_failsafe[0]);
    TEST_ASSERT_EQUAL(GW_DO_SAFE_ON, cfg.local_io.do_failsafe[1]);
    TEST_ASSERT_EQUAL(GW_DO_SAFE_OFF, cfg.local_io.do_failsafe[2]);
    TEST_ASSERT_EQUAL(GW_DO_SAFE_HOLD, cfg.local_io.do_failsafe[3]);
    TEST_ASSERT_EQUAL(GW_DO_SAFE_OFF, cfg.local_io.do_failsafe[4]);   /* missing: off */
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"failsafe_s\":86401}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.failsafe_s: valor 86401 fuera de rango [0, 86400]");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"do_failsafe\":[\"off\",\"keep\"]}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.do_failsafe[1]: valor no válido (se esperaba off|on|hold)");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"do_failsafe\":[true]}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.do_failsafe[0]: valor no válido");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"do_failsafe\":\"off\"}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.do_failsafe: se esperaba un array");
    snprintf(doc, sizeof(doc),
             "{%s,\"local_io\":{\"do_failsafe\":[\"off\",\"off\",\"off\",\"off\",\"off\","
             "\"off\",\"off\",\"off\",\"off\"]}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.do_failsafe: 9 entradas; hay 8 salidas");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"counter_save_s\":0}}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL(0, cfg.local_io.counter_save_s);
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"counter_save_s\":59}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.counter_save_s: valor 59: 0 (no guardar) o al menos 60 s");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"counter_save_s\":86401}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.counter_save_s: valor 86401 fuera de rango");
    /* IO-6 */
    snprintf(doc, sizeof(doc), "{%s}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL(0, cfg.local_io.buzzer.alarms);              /* silent */
    TEST_ASSERT_EQUAL(30, cfg.local_io.buzzer.alarm_after_s);
    snprintf(doc, sizeof(doc),
             "{%s,\"local_io\":{\"buzzer\":{\"alarms\":[\"mqtt_down\",\"config_error\"],"
             "\"alarm_after_s\":5}}}", base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL_HEX8(GW_ALARM_BIT(GW_ALARM_MQTT_DOWN) | GW_ALARM_BIT(GW_ALARM_CONFIG_ERROR),
                           cfg.local_io.buzzer.alarms);
    TEST_ASSERT_EQUAL(5, cfg.local_io.buzzer.alarm_after_s);
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"buzzer\":{\"alarms\":[\"fire\"]}}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.buzzer.alarms[0]: valor no válido");
    snprintf(doc, sizeof(doc),
             "{%s,\"local_io\":{\"buzzer\":{\"alarms\":[\"mqtt_down\",\"mqtt_down\"]}}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.buzzer.alarms[1]: alarma repetida");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"buzzer\":{\"alarm_after_s\":3601}}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.buzzer.alarm_after_s: valor 3601 fuera de rango");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"buzzer\":{\"volume\":1}}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.buzzer.volume: clave desconocida");
    /* IO-7 */
    snprintf(doc, sizeof(doc),
             "{%s,\"local_io\":{\"do_failsafe\":[\"hold\"],\"rules\":[{\"di\":2,\"do\":1},"
             "{\"di\":2,\"do\":8,\"mode\":\"invert\"},{\"di\":3,\"do\":4,\"mode\":\"toggle\"}]}}",
             base);
    TEST_ASSERT_TRUE_MESSAGE(parse(doc), err.msg);
    TEST_ASSERT_EQUAL(3, cfg.local_io.rule_count);
    TEST_ASSERT_EQUAL(1, cfg.local_io.rules[0].di);
    TEST_ASSERT_EQUAL(0, cfg.local_io.rules[0].dout);
    TEST_ASSERT_EQUAL(GW_RULE_FOLLOW, cfg.local_io.rules[0].mode);
    TEST_ASSERT_EQUAL(7, cfg.local_io.rules[1].dout);
    TEST_ASSERT_EQUAL(GW_RULE_INVERT, cfg.local_io.rules[1].mode);
    TEST_ASSERT_EQUAL(GW_RULE_TOGGLE, cfg.local_io.rules[2].mode);
    snprintf(doc, sizeof(doc),
             "{%s,\"local_io\":{\"rules\":[{\"di\":1,\"do\":3},{\"di\":2,\"do\":3}]}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.rules[1].do: la DO/3 ya la maneja local_io.rules[0]");
    snprintf(doc, sizeof(doc),
             "{%s,\"local_io\":{\"do_failsafe\":[\"off\",\"on\"],\"rules\":[{\"di\":1,\"do\":2}]}}",
             base);
    ASSERT_REJECTED(parse(doc), "local_io.do_failsafe[1]: la DO/2 la maneja local_io.rules[0]");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"rules\":[{\"di\":9,\"do\":1}]}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.rules[0].di: valor 9 fuera de rango [1, 8]");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"rules\":[{\"di\":1}]}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.rules[0].do: campo obligatorio ausente");
    snprintf(doc, sizeof(doc),
             "{%s,\"local_io\":{\"rules\":[{\"di\":1,\"do\":1,\"mode\":\"latch\"}]}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.rules[0].mode: valor no válido (se esperaba follow|invert|toggle)");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"rules\":[{\"di\":1,\"do\":1,\"when\":1}]}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.rules[0].when: clave desconocida");
    snprintf(doc, sizeof(doc), "{%s,\"local_io\":{\"rules\":{}}}", base);
    ASSERT_REJECTED(parse(doc), "local_io.rules: se esperaba un array");
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
                    "extra: clave desconocida");
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

static void test_syntax_errors_give_line_and_column(void)
{
    ASSERT_REJECTED(parse("{\n  \"mqtt\": {\"host\": \"b\"}\n  \"sparkplug\": {}\n}"),
                    "JSON mal formado en la línea 3, columna 3");  /* missing ',' */
    ASSERT_REJECTED(parse("{\"mqtt\": {\"host\": tru}}"), "línea 1, columna 19");
    /* columns count characters, not UTF-8 bytes */
    ASSERT_REJECTED(parse("{\"mqtt\": {\"host\": \"\xc3\xa1\" x}}"), "línea 1, columna 23");
    ASSERT_REJECTED(parse("{\"mqtt\": {\"host\": \"b\"}, \"devices\": [\n"), "línea 1");
    ASSERT_REJECTED(parse("{\"mqtt\": {\"host\": \"b\"}}\n\n  x"),
                    "línea 3, columna 3: contenido después del documento");
    TEST_ASSERT_NULL(strstr(err.msg, "$"));
}

static void test_semantic_errors_give_json_path(void)
{
    static const char doc[] =
        "{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"},"
        "\"devices\":[{\"name\":\"A\",\"unit_id\":1,\"tags\":[{\"name\":\"t\",\"area\":\"coil\","
        "\"address\":0,\"type\":\"bool\"}]},"
        "{\"name\":\"B\",\"unit_id\":2,\"tags\":["
        "{\"name\":\"a\",\"area\":\"input\",\"address\":0,\"type\":\"int16\"},"
        "{\"name\":\"b\",\"area\":\"input\",\"address\":1,\"type\":\"int16\"},"
        "{\"name\":\"c\",\"area\":\"input\",\"address\":2,\"type\":\"int8\"}]}]}";
    ASSERT_REJECTED(parse(doc), "devices[1].tags[2].type: valor no válido");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":[]}"),
                    "sparkplug: se esperaba un objeto JSON");
    ASSERT_REJECTED(parse("[]"), "$: se esperaba un objeto JSON");
}

static void test_duplicate_keys_rejected(void)
{
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"a\",\"port\":1883,\"host\":\"b\"},"
                          "\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"}}"),
                    "mqtt.host: clave duplicada");
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"input\",\"address\":0,\"type\":\"int16\","
                              "\"address\":4}"),
                    "devices[0].tags[0].address: clave duplicada");
    ASSERT_REJECTED(parse("{\"mqtt\":{\"host\":\"b\"},\"sparkplug\":{\"group_id\":\"G\","
                          "\"edge_node_id\":\"E\"},\"devices\":[],\"devices\":[]}"),
                    "devices: clave duplicada");
}

/* SPEC maximum: 16 devices x 16 tags carrying every optional field (CFG-1 with H1 of the
 * IDF review: a legal document larger than the old 32 KB file limit). */
static size_t build_spec_maximum(char *doc, size_t cap)
{
    int n = snprintf(doc, cap,
                     "{\"network\":{\"dhcp\":true},\"ntp\":{\"server\":\"pool.ntp.org\"},"
                     "\"mqtt\":{\"host\":\"192.168.1.10\",\"port\":1883,\"client_id\":\"\","
                     "\"username\":\"\",\"password\":\"\",\"keepalive_s\":30},"
                     "\"sparkplug\":{\"group_id\":\"Plant1\",\"edge_node_id\":\"GW01\"},"
                     "\"devices\":[");
    for (int d = 0; d < GW_MAX_DEVICES; d++) {
        n += snprintf(doc + n, cap - (size_t)n,
                      "%s{\"name\":\"Device%02d\",\"unit_id\":%d,\"poll_ms\":1000,"
                      "\"offline_after\":3,\"tags\":[",
                      d ? "," : "", d, d + 1);
        for (int t = 0; t < GW_MAX_TAGS / GW_MAX_DEVICES; t++) {
            n += snprintf(doc + n, cap - (size_t)n,
                          "%s{\"name\":\"Tag_%02d_%02d_ActivePowerPhaseL1\",\"area\":\"input\","
                          "\"address\":%d,\"type\":\"float32\",\"word_order\":\"big\","
                          "\"scale\":1.0,\"offset\":0.0,\"deadband\":0.1,\"writable\":false,"
                          "\"units\":\"kW\"}",
                          t ? "," : "", d, t, 2 * t);
        }
        n += snprintf(doc + n, cap - (size_t)n, "]}");
    }
    n += snprintf(doc + n, cap - (size_t)n, "]}");
    return (size_t)n;
}

static void test_spec_maximum_document(void)
{
    static char doc[96 * 1024];
    size_t len = build_spec_maximum(doc, sizeof(doc));
    TEST_ASSERT_GREATER_THAN(40 * 1024, len);
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(doc, len, &cfg, &err), err.msg);
    TEST_ASSERT_EQUAL(GW_MAX_DEVICES, cfg.device_count);
    TEST_ASSERT_EQUAL(GW_MAX_TAGS, cfg.tag_count);
}

static size_t s_allocs;
static size_t s_fail_after;

static void *counting_alloc(size_t size)
{
    return ++s_allocs > s_fail_after ? NULL : malloc(size);
}

static void test_allocator_and_out_of_memory(void)
{
    gw_config_set_allocator(counting_alloc, free);
    s_allocs = 0;
    s_fail_after = SIZE_MAX;
    TEST_ASSERT_TRUE_MESSAGE(parse_tag("{\"name\":\"t\",\"area\":\"coil\",\"address\":0,"
                                       "\"type\":\"bool\"}"),
                             err.msg);
    TEST_ASSERT_GREATER_THAN(10, s_allocs);  /* the document tree went through the hook */
    s_allocs = 0;
    s_fail_after = 5;
    ASSERT_REJECTED(parse_tag("{\"name\":\"t\",\"area\":\"coil\",\"address\":0,"
                              "\"type\":\"bool\"}"),
                    "memoria insuficiente");
    TEST_ASSERT_NULL(strstr(err.msg, "mal formado"));
    gw_config_set_allocator(malloc, free);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_spec_example);
    RUN_TEST(test_embedded_default_is_valid);
    RUN_TEST(test_defaults_and_static_ip);
    RUN_TEST(test_wifi_section);
    RUN_TEST(test_ipv6_literals);
    RUN_TEST(test_ipv6_section);
    RUN_TEST(test_syslog_section);
    RUN_TEST(test_tls_section);
    RUN_TEST(test_tls_client_certificate);
    RUN_TEST(test_backup_brokers);
    RUN_TEST(test_mqtt_protocol);
    RUN_TEST(test_store_forward_key);
    RUN_TEST(test_web_section);
    RUN_TEST(test_web_https);
    RUN_TEST(test_hostname);
    RUN_TEST(test_error_fields);
    RUN_TEST(test_schema_rules);
    RUN_TEST(test_mb6_tags);
    RUN_TEST(test_eng_range);
    RUN_TEST(test_bytes_tags);
    RUN_TEST(test_device_types);
    RUN_TEST(test_utf8_sequences);
    RUN_TEST(test_sparkplug_names);
    RUN_TEST(test_device_prefix);
    RUN_TEST(test_mdns_instance);
    RUN_TEST(test_lint_name_rules);
    RUN_TEST(test_lint_umh_preset);
    RUN_TEST(test_device_block_reads);
    RUN_TEST(test_scan_classes);
    RUN_TEST(test_modbus_tcp_section);
    RUN_TEST(test_ota_section);
    RUN_TEST(test_local_io_section);
    RUN_TEST(test_section_errors);
    RUN_TEST(test_limits);
    RUN_TEST(test_syntax_errors_give_line_and_column);
    RUN_TEST(test_semantic_errors_give_json_path);
    RUN_TEST(test_duplicate_keys_rejected);
    RUN_TEST(test_spec_maximum_document);
    RUN_TEST(test_allocator_and_out_of_memory);
    return UNITY_END();
}
