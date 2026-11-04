/* QA-1: configuration parsing and validation (CFG-1, SPEC section 6). */
#include <stdio.h>
#include <stdlib.h>
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
    RUN_TEST(test_web_section);
    RUN_TEST(test_hostname);
    RUN_TEST(test_error_fields);
    RUN_TEST(test_schema_rules);
    RUN_TEST(test_section_errors);
    RUN_TEST(test_limits);
    RUN_TEST(test_syntax_errors_give_line_and_column);
    RUN_TEST(test_semantic_errors_give_json_path);
    RUN_TEST(test_duplicate_keys_rejected);
    RUN_TEST(test_spec_maximum_document);
    RUN_TEST(test_allocator_and_out_of_memory);
    return UNITY_END();
}
