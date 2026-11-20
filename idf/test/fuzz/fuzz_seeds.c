/*
 * Issue #226: the seed corpus of the fuzz targets. Each seed is a valid input, made with the
 * encoders of gw_core (so that it follows a change of a format) or with the configuration
 * documents of the repository:
 *
 *   fuzz_seeds <dir>      writes <dir>/<target>/<name> for each target
 */
#include <errno.h>
#include <sys/stat.h>

#include "fuzz.h"
#include "fuzz_card.h"
#include "gw_auth.h"
#include "gw_captive.h"
#include "gw_cfgprev.h"
#include "gw_config.h"
#include "gw_fwlast.h"
#include "gw_lkgnet.h"
#include "gw_node.h"
#include "gw_nvsraw.h"
#include "gw_ota.h"
#include "gw_port.h"
#include "gw_rtc.h"
#include "gw_sf.h"
#include "gw_sfspill.h"
#include "gw_tagdb.h"
#include "mb_rtu.h"
#include "spb.h"

static const char *s_dir;
static char s_target[32];
static unsigned s_count;

static void mkdir_ok(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        perror(path);
        exit(1);
    }
}

static void target(const char *name)
{
    snprintf(s_target, sizeof(s_target), "%s", name);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    mkdir_ok(path);
}

/* One seed: the prefix bytes, then the data. */
static void seed(const char *name, const uint8_t *prefix, size_t plen, const void *data,
                 size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", s_dir, s_target, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL || fwrite(prefix, 1, plen, f) != plen ||
        (len != 0 && fwrite(data, 1, len, f) != len)) {
        perror(path);
        exit(1);
    }
    fclose(f);
    s_count++;
}

static void seed1(const char *name, uint8_t sel, const void *data, size_t len)
{
    seed(name, &sel, 1, data, len);
}

static void seed_str(const char *name, uint8_t sel, const char *s)
{
    seed1(name, sel, s, strlen(s));
}

static gw_config_t s_cfg_spec, s_cfg_e2e;
static char *s_doc_spec, *s_doc_e2e, *s_doc_default;
static size_t s_len_spec, s_len_e2e, s_len_default;

static void load_docs(void)
{
    s_doc_spec = fz_read_file(GW_FUZZ_SPEC_EXAMPLE, &s_len_spec);
    s_doc_e2e = fz_read_file(GW_FUZZ_E2E_CONFIG, &s_len_e2e);
    s_doc_default = fz_read_file(GW_FUZZ_DEFAULT_CONFIG, &s_len_default);
    gw_cfg_err_t err;
    if (!gw_config_parse(s_doc_spec, s_len_spec, &s_cfg_spec, &err) ||
        !gw_config_parse(s_doc_e2e, s_len_e2e, &s_cfg_e2e, &err)) {
        fprintf(stderr, "a configuration of the repository is rejected: %s\n", err.msg);
        exit(1);
    }
}

/* ---- the RS485 bus ---- */

static size_t rtu(uint8_t *out, const uint8_t *body, size_t len)
{
    memcpy(out, body, len);
    uint16_t crc = mb_crc16(out, len);
    out[len] = (uint8_t)crc;
    out[len + 1] = (uint8_t)(crc >> 8);
    return len + 2;
}

static void seeds_mb_rtu(void)
{
    target("fuzz_mb_rtu");
    uint8_t b[64];
    /* sel (fc index), unit, addr, qty, value, retries; then the response */
    uint8_t hdr[10] = {2, 1, 0, 10, 0, 2, 0, 0, 0};
    size_t n = rtu(b, (const uint8_t[]){1, 3, 4, 0x12, 0x34, 0xBE, 0xEF}, 7);
    seed("fc03", hdr, 10, b, n);
    hdr[0] = 2 | 0x80;   /* through mb_master */
    seed("fc03_master", hdr, 10, b, n);
    n = rtu(b, (const uint8_t[]){1, 0x83, 0x02}, 3);
    hdr[0] = 2;
    seed("fc03_exception", hdr, 10, b, n);
    uint8_t coils[10] = {0, 1, 0, 0, 0, 10, 0, 0, 0};
    n = rtu(b, (const uint8_t[]){1, 1, 2, 0x55, 0x01}, 5);
    seed("fc01", coils, 10, b, n);
    uint8_t wr[10] = {5, 1, 0, 10, 0, 1, 0x12, 0x34, 0};
    n = rtu(b, (const uint8_t[]){1, 6, 0, 10, 0x12, 0x34}, 6);
    seed("fc06", wr, 10, b, n);
    uint8_t wm[10] = {7, 1, 0, 10, 0, 2, 0, 0, 0};
    n = rtu(b, (const uint8_t[]){1, 0x10, 0, 10, 0, 2}, 6);
    seed("fc16", wm, 10, b, n);
}

/* ---- Modbus TCP ---- */

static void seeds_mbap(void)
{
    target("fuzz_mbap");
    /* di, dout, flags (DOs known), rule_do, 8 counters; then the stream length and the stream */
    uint8_t in[256];
    size_t p = 0;
    in[p++] = 0x05;
    in[p++] = 0x02;
    in[p++] = 0x01;
    in[p++] = 0x00;
    for (int i = 0; i < 8; i++) {
        in[p++] = (uint8_t)i;
    }
    const uint8_t stream[] = {
        0x00, 0x01, 0, 0, 0, 6, 1, 0x03, 0x00, 0x0A, 0x00, 0x02,     /* unit 1, FC03 */
        0x00, 0x02, 0, 0, 0, 6, 100, 0x04, 0x00, 0x00, 0x00, 0x08,   /* local unit, FC04 */
        0x00, 0x03, 0, 0, 0, 6, 100, 0x05, 0x00, 0x01, 0xFF, 0x00,   /* local unit, FC05 */
        0x00, 0x04, 0, 0, 0, 6, 100, 0x01, 0x00, 0x00, 0x00, 0x10,   /* local unit, FC01 */
    };
    in[p++] = 0;
    in[p++] = sizeof(stream);
    memcpy(&in[p], stream, sizeof(stream));
    p += sizeof(stream);
    p += rtu(&in[p], (const uint8_t[]){1, 3, 4, 0x12, 0x34, 0xBE, 0xEF}, 7);
    seed("requests", in, p, NULL, 0);
}

/* ---- MQTT ---- */

static uint8_t s_pay[GW_SPB_PAYLOAD_MAX];
static unsigned s_published;

static int cap_publish(void *ctx, const char *topic, const uint8_t *data, size_t len, int qos,
                       bool retain)
{
    (void)ctx;
    (void)qos;
    (void)retain;
    char name[96];
    const char *type = strstr(topic, "BIRTH") ? "birth" : strstr(topic, "DEATH") ? "death" : "data";
    snprintf(name, sizeof(name), "node_%s_%u", type, s_published++);
    seed1(name, 0, data, len);
    return 0;
}

static bool nop_set_do(void *ctx, uint8_t index, bool on)
{
    (void)ctx;
    (void)index;
    (void)on;
    return true;
}

static bool nop_request_write(void *ctx, uint16_t tag, const gw_tag_value_t *value)
{
    (void)ctx;
    (void)tag;
    (void)value;
    return true;
}

static size_t nop_write_room(void *ctx)
{
    (void)ctx;
    return 64;
}

static void nop_set_scan_rate(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
}

static void nop_reboot(void *ctx)
{
    (void)ctx;
}

static size_t encode(const spb_metric_t *m, size_t n, uint8_t *buf, size_t cap)
{
    spb_payload_t p = {.timestamp = 1700000000000ull, .metrics = m, .metric_count = n};
    size_t len = spb_encode(&p, buf, cap);
    if (len == 0) {
        fprintf(stderr, "spb_encode failed\n");
        exit(1);
    }
    return len;
}

static size_t s_rebirth_len, s_dcmd_len, s_fw_len;
static uint8_t s_rebirth[64], s_dcmd[128], s_fw[512];

static void seeds_spb(void)
{
    target("fuzz_spb");
    /* the births and data of a node with every feature of the E2E configuration */
    gw_port_fake_time(1700000000000ull);
    gw_tagdb_reset();
    gw_tag_value_t vals[GW_MAX_TAGS_PER_DEVICE];
    memset(vals, 0, sizeof(vals));
    for (size_t i = 0; i < GW_MAX_TAGS_PER_DEVICE; i++) {
        vals[i].valid = true;
        vals[i].ts_ms = 1000;
    }
    for (uint16_t d = 0; d < s_cfg_e2e.device_count; d++) {
        gw_tagdb_put_device(&s_cfg_e2e, d, vals, true);
    }
    gw_node_ops_t ops = {.publish = cap_publish, .set_do = nop_set_do,
                         .request_write = nop_request_write, .write_room = nop_write_room,
                         .set_scan_rate = nop_set_scan_rate, .reboot = nop_reboot};
    const gw_node_info_t info = {.firmware = "0.1.0", .hardware = "seed", .reset_reason = "-"};
    static gw_node_t node;
    gw_node_init(&node, &s_cfg_e2e, &ops, &info);
    gw_node_set_buffer(&node, s_pay, sizeof(s_pay));
    gw_node_session_up(&node, 3);
    gw_tagdb_set_di(0x01, 2000);
    gw_node_io_changed(&node);
    uint8_t death[64];
    size_t dl = gw_node_death_payload(3, death, sizeof(death));
    seed1("node_ndeath", 0, death, dl);

    /* commands */
    spb_metric_t rebirth = {.name = "Node Control/Rebirth", .datatype = SPB_DT_BOOLEAN,
                            .v.num = 1};
    s_rebirth_len = encode(&rebirth, 1, s_rebirth, sizeof(s_rebirth));
    seed1("ncmd_rebirth", 0, s_rebirth, s_rebirth_len);
    seed1("ncmd_rebirth_validate", 16, s_rebirth, s_rebirth_len);
    spb_metric_t dcmd[3] = {
        {.name = "Setpoint", .datatype = SPB_DT_FLOAT, .v.num = 12.5},
        {.alias = GW_ALIAS_TAG_BASE + 2, .datatype = SPB_DT_BOOLEAN, .v.num = 1},
        {.name = "Label", .datatype = SPB_DT_STRING, .v.str = "text"},
    };
    s_dcmd_len = encode(dcmd, 3, s_dcmd, sizeof(s_dcmd));
    seed1("dcmd", 0, s_dcmd, s_dcmd_len);
    spb_metric_t fw[2] = {
        {.name = "Node Control/Firmware SHA-256", .datatype = SPB_DT_STRING,
         .v.str = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
        {.name = "Node Control/Firmware Update", .datatype = SPB_DT_STRING,
         .v.str = "https://fw.example/gw.bin"},
    };
    s_fw_len = encode(fw, 2, s_fw, sizeof(s_fw));
    seed1("ncmd_firmware", 0, s_fw, s_fw_len);
    static const uint8_t bytes[4] = {1, 2, 3, 4};
    spb_metric_t misc[3] = {
        {.name = "Node Control/Scan Rate", .datatype = SPB_DT_INT64, .v.i64 = 1000},
        {.name = "b", .datatype = SPB_DT_BYTES, .v.bytes = {bytes, sizeof(bytes)}},
        {.name = "n", .datatype = SPB_DT_DOUBLE, .is_null = true},
    };
    uint8_t buf[256];
    seed1("misc", 0, buf, encode(misc, 3, buf, sizeof(buf)));
    /* #251: a Template instance with a member and a parameter of each value field; with the
     * param callback, in a validation, and with the decoder of gw_node (no param callback) */
    const spb_metric_t member = {.name = "Setpoint", .no_alias = true, .datatype = SPB_DT_FLOAT,
                                 .v.num = 12.5};
    const spb_param_t params[] = {
        {.name = "Unit ID", .datatype = SPB_DT_INT32, .v.num = 2},
        {.name = "Serial", .datatype = SPB_DT_UINT64, .v.u64 = 1234567890123ull},
        {.name = "Gain", .datatype = SPB_DT_FLOAT, .v.num = 1.5},
        {.name = "Span", .datatype = SPB_DT_DOUBLE, .v.num = -0.25},
        {.name = "Spare", .datatype = SPB_DT_BOOLEAN, .v.num = 1},
        {.name = "Model", .datatype = SPB_DT_STRING, .v.str = "PM-3"},
    };
    const spb_template_t inst = {.ref = "Pump", .count = 1, .members = &member,
                                 .param_count = sizeof(params) / sizeof(params[0]),
                                 .params = params};
    const spb_metric_t tmpl = {.name = "Pump", .alias = GW_ALIAS_TAG_BASE,
                               .datatype = SPB_DT_TEMPLATE, .v.tmpl = &inst};
    size_t tl = encode(&tmpl, 1, buf, sizeof(buf));
    seed1("template_params", 0, buf, tl);
    seed1("template_params_validate", 16, buf, tl);
    seed1("template_params_skipped", 32, buf, tl);
}

static void node_msg(uint8_t *out, size_t *p, uint8_t kind, const uint8_t *pay, size_t len)
{
    out[(*p)++] = kind;
    out[(*p)++] = (uint8_t)(len >> 8);
    out[(*p)++] = (uint8_t)len;
    memcpy(&out[*p], pay, len);
    *p += len;
}

static void seeds_node(void)
{
    target("fuzz_node");
    static uint8_t in[2048];
    size_t p = 0;
    in[p++] = 0x03;   /* the E2E configuration, device 0 online */
    node_msg(in, &p, 0, s_rebirth, s_rebirth_len);
    node_msg(in, &p, 1, s_dcmd, s_dcmd_len);
    node_msg(in, &p, 0, s_fw, s_fw_len);
    const char *state = "{\"online\":true,\"timestamp\":1700000000000}";
    node_msg(in, &p, 3, (const uint8_t *)state, strlen(state));
    seed("commands_e2e", NULL, 0, in, p);
    in[0] = 0x02;      /* the example of SPEC section 6 */
    seed("commands_spec", NULL, 0, in, p);
}

/* ---- the configuration ---- */

static void seeds_config(const char *name)
{
    target(name);
    seed_str("spec_example", 0, s_doc_spec);
    seed_str("default_config", 0, s_doc_default);
    seed_str("e2e_config", 0, s_doc_e2e);
    seed_str("e2e_config_schema", 64, s_doc_e2e);
    if (strcmp(name, "fuzz_cfgdoc") == 0) {
        seed_str("e2e_config_update", 64, s_doc_e2e);
        const char *secrets = "{\"mqtt\":{\"host\":\"h\",\"password\":\"********\"},"
                              "\"network\":{\"wifi\":{\"ssid\":\"s\",\"password\":\"pw123456\"}},"
                              "\"web\":{\"password\":\"new password\"}}";
        seed_str("secrets", 0, secrets);
        seed_str("secrets_update", 64, secrets);
    }
}

/* fuzz_web_json: the selector, the option byte, then the body */
static void seed_body(const char *name, uint8_t sel, const char *body)
{
    const uint8_t prefix[2] = {sel, 0};
    seed(name, prefix, 2, body, strlen(body));
}

static void seeds_web_json(void)
{
    target("fuzz_web_json");
    seed_body("claim", 0, "{\"password\": \"a good password\"}");
    seed_body("mbdiag", 1, "{\"unit\":1,\"area\":\"holding\",\"address\":10,\"count\":2}");
    seed_body("mbdiag_type", 1,
             "{\"unit\":1,\"area\":\"input\",\"address\":0,\"type\":\"float32\"}");
    seed_body("iocmd", 2, "{\"do\": 3, \"on\": true}");
    seed_body("ota", 3, "{\"url\": \"https://fw.example/gw.bin\", \"force\": true, \"sha256\": "
                       "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}");
    seed_body("loglevel", 4, "{\"tag\": \"poll\", \"level\": \"debug\"}");
    seed_body("loglevel_reset", 4, "{\"reset\": true}");
    seed_body("state", 5, "{\"online\":true,\"timestamp\":1700000000000}");
}

static void seeds_web_text(void)
{
    target("fuzz_web_text");
    uint8_t salt[GW_AUTH_SALT_LEN] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    char hash[GW_PWHASH_SIZE];
    if (!gw_auth_hash_password("pw", salt, 10, hash, sizeof(hash))) {
        exit(1);
    }
    char two[GW_PWHASH_SIZE + 4];
    snprintf(two, sizeof(two), "pw%c%s", 0, hash);
    seed1("password", 2, two, 3 + strlen(hash));
    seed_str("basic", 0xF0, "Basic YWRtaW46c2VjcmV0");   /* admin:secret */
    seed_str("base64", 0xF1, "YWRtaW46c2VjcmV0");
    const char csrf[] = "gw01.local:8443\0https://gw01.local:8443\0\0fetch";
    seed1("csrf", 0x13, csrf, sizeof(csrf) - 1);
    seed_str("url", 4, "https://fw.example:8443/path/gw.bin?x=1");
    seed_str("sha256", 5, "0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef");
    const char san[] = "DNS:gw01.local,DNS:gw01,IP:192.168.1.50\0gw01";
    seed1("san", 6, san, sizeof(san) - 1);
    seed_str("wpa", 7, "a good passphrase");
    seed_str("pem", 8, "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n"
                       "-----BEGIN PRIVATE KEY-----\nMIIE\n-----END PRIVATE KEY-----\n");
    seed_str("file", 9, "ca.pem");
}

/* ---- the flash, the RTC memory and an image ---- */

static void seeds_flash(void)
{
    target("fuzz_flash");
    uint8_t buf[GW_LKG_MAX];
    size_t n = gw_lkg_encode(&s_cfg_e2e, buf, sizeof(buf));
    seed1("lkg_e2e", 0, buf, n);
    n = gw_lkg_encode(&s_cfg_spec, buf, sizeof(buf));
    seed1("lkg_spec", 0, buf, n);
    gw_fwlast_t r = {.result = GW_FWLAST_ROLLED_BACK, .expect = GW_OTA_MODE_NORMAL};
    snprintf(r.version, sizeof(r.version), "1.2.3+gabcdef0");
    snprintf(r.cause, sizeof(r.cause), "configuration rejected: a.b");
    char text[GW_FWLAST_MAX];
    gw_fwlast_format(&r, text, sizeof(text));
    seed_str("fwlast", 1, text);
    gw_cfgprev_t p = {.schema = 2, .saved = 1700000000, .hash = 0x12345678};
    snprintf(p.armed, sizeof(p.armed), "1.2.3");
    snprintf(p.version, sizeof(p.version), "1.2.2");
    char ptext[GW_CFGPREV_MAX];
    gw_cfgprev_format(&p, ptext, sizeof(ptext));
    seed_str("cfgprev", 2, ptext);
    uint8_t regs[GW_RTC_REGS];
    gw_rtc_encode(1763469296, regs);
    seed1("rtc", 3, regs, sizeof(regs));
    gw_ota_caps_t caps = {.magic = GW_OTA_CAPS_MAGIC, .version = GW_OTA_CAPS_VERSION,
                          .size = sizeof(gw_ota_caps_t), .flags = GW_OTA_CAP_NVS_ENCRYPTION,
                          .cfg_schema = GW_CONFIG_SCHEMA, .nvs_format = 0xFE01u,
                          .lfs_disk = 0x00020001u};
    seed1("caps", 4, &caps, sizeof(caps));
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* An NVS entry: namespace index, type, span 1, chunk index 0xFF, CRC, key and 8 data bytes. */
static void nvs_entry(uint8_t *page, uint32_t i, uint8_t ns, uint8_t type, const char *key,
                      uint8_t value)
{
    uint8_t *e = page + 64u + 32u * i;
    memset(e, 0xFF, 32);
    e[0] = ns;
    e[1] = type;
    e[2] = 1;
    e[3] = 0xFF;
    memset(e + 8, 0, 16);
    memcpy(e + 8, key, strlen(key));
    memset(e + 24, 0xFF, 8);
    e[24] = value;
    uint32_t c = gw_nvsraw_crc32(0xFFFFFFFFu, e, 4);
    c = gw_nvsraw_crc32(c, e + 8, 16);
    c = gw_nvsraw_crc32(c, e + 24, 8);
    put32(e + 4, c);
    /* the entry state bitmap: 2 bits per entry, 10 = written */
    uint8_t *bm = page + 32u + (i / 4u);
    *bm = (uint8_t)(*bm & ~(1u << ((i % 4u) * 2u)));
}

static void seeds_nvs(void)
{
    target("fuzz_nvs");
    /* a raw page: active, with the namespace "gw" (index 1) and its u8 "boot_count" */
    static uint8_t page[GW_NVSRAW_PAGE];
    memset(page, 0xFF, sizeof(page));
    put32(page, 0xFFFFFFFEu);   /* active */
    put32(page + 4, 0);         /* sequence number */
    page[8] = 0xFE;             /* version 2 */
    memset(page + 9, 0xFF, 19);
    put32(page + 28, gw_nvsraw_crc32(0xFFFFFFFFu, page + 4, 24));
    nvs_entry(page, 0, 0, 0x01, "gw", 1);
    nvs_entry(page, 1, 1, 0x01, "boot_count", 3);
    nvs_entry(page, 2, 0, 0x01, "gw_mig", 2);
    nvs_entry(page, 3, 2, 0x01, GW_NVSMIG_ERASE_KEY, GW_NVSMIG_ERASE_VALUE);
    seed1("raw_page", 1, page, sizeof(page));
    seed1("raw_page_fix", 3, page, sizeof(page));
    /* a sealed copy of a migration */
    uint8_t copy[512];
    size_t used = gw_nvsmig_begin(copy, sizeof(copy));
    uint8_t one = 3;
    used = gw_nvsmig_add(copy, sizeof(copy), used, "gw", "boot_count", 0x01, &one, 1);
    used = gw_nvsmig_add(copy, sizeof(copy), used, "gw", "user", 0x21, "admin", 6);
    uint32_t big = 0x12345678;
    used = gw_nvsmig_add(copy, sizeof(copy), used, "spb", "bdseq", 0x04, &big, 4);
    if (used == 0) {
        exit(1);
    }
    gw_nvsmig_seal(copy, used, 1);
    seed1("copy", 0, copy, used);
    seed1("copy_fix", 2, copy, used);
}

/* ---- text ---- */

static void seeds_text(void)
{
    target("fuzz_text");
    seed_str("ipv6", 0, "2001:db8::1");
    seed_str("ipv6_v4", 0, "::ffff:192.0.2.1");
    seed_str("ipv6_full", 0, "fe80:0:0:0:200:5eff:fe00:5301");
    seed_str("hostname", 0, "Plant 1/GW01 \xc3\xa9");
    seed_str("file", 0, "ca-chain.pem");
    seed_str("loglevel", 0, "verbose");
    seed_str("log", 1, "\x1b[0;32mI (1234) edge: connected to mqtt://10.0.2.2:1883\x1b[0m\n");
    seed_str("log_time", 1, "W (12:34:56.789) bus: unit 3: timeout");
}

/* ---- the microSD card ---- */

static card_t s_card;
static uint32_t s_ms;

static uint32_t clock_ms(void)
{
    return s_ms += 10;
}

static void sleep_ms(uint32_t ms)
{
    s_ms += ms;
}

static uint32_t s_n;
static uint32_t nonce(void)
{
    return ++s_n * 0x9E3779B9u;
}

static void seeds_sfspill(void)
{
    target("fuzz_sfspill");
    static uint64_t mem[(16384 + 7) / 8];
    static gw_sf_rec_t ram[64];
    static gw_sfspill_t sp;
    static uint8_t img[CARD_IMAGE_SIZE];
    card_init(&s_card);
    gw_sfspill_io_t io = card_io(&s_card, 4096);
    gw_sfspill_init(&sp, &io, CARD_EXT_BLOCKS, CARD_EXTENTS, gw_sfspill_cfg_hash(&s_cfg_spec),
                    mem);
    sp.random = nonce;
    gw_sf_t sf;
    gw_sf_init(&sf, ram, 64);
    gw_sf_attach_spill(&sf, &sp);
    for (int i = 0; i < 100 && (sp.phase != GW_SFSPILL_READY || sp.sb_due); i++) {
        gw_sf_step(&sf, clock_ms());
    }
    for (int i = 0; i < 40; i++) {
        if (i % 8 == 0) {
            gw_sf_begin(&sf);
        }
        if (i % 13 == 0) {
            gw_sf_push_str(&sf, 0, 0, "a stored string", false, 1000u + (uint64_t)i);
        } else {
            gw_sf_push(&sf, i % 5 == 0 ? GW_SF_NODE : 0, (uint16_t)(i % 3), i, i % 7 == 0,
                       1000u + (uint64_t)i);
        }
    }
    gw_sf_flush_t res;
    gw_sf_flush(&sf, clock_ms, sleep_ms, 60000, &res);
    size_t n = card_save(&s_card, img, sizeof(img));
    seed("card", NULL, 0, img, n);
    img[0] |= 0x60;   /* the same, with the CRCs corrected */
    seed("card_fix", NULL, 0, img, n);
}

/* ---- the captive portal of the access point (#274) ---- */

/* A DNS query for @p name with @p qtype, ID 0xBEEF, RD set; @p opt adds an EDNS OPT record. */
static size_t dns_query(uint8_t *q, const char *name, unsigned qtype, bool opt)
{
    static const uint8_t hdr[12] = {0xBE, 0xEF, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    memcpy(q, hdr, sizeof(hdr));
    size_t n = sizeof(hdr);
    for (const char *s = name; *s != '\0';) {
        size_t l = strcspn(s, ".");
        q[n++] = (uint8_t)l;
        memcpy(&q[n], s, l);
        n += l;
        s += l + (s[l] == '.' ? 1u : 0u);
    }
    q[n++] = 0;
    q[n++] = (uint8_t)(qtype >> 8);
    q[n++] = (uint8_t)qtype;
    q[n++] = 0;
    q[n++] = 1;   /* class IN */
    if (opt) {
        static const uint8_t rr[11] = {0, 0, 41, 0x04, 0xD0, 0, 0, 0, 0, 0, 0};
        memcpy(&q[n], rr, sizeof(rr));
        n += sizeof(rr);
        q[11] = 1;
    }
    return n;
}

static void seeds_captive(void)
{
    target("fuzz_captive");
    uint8_t in[300];
    static const uint8_t ip[4] = {192, 168, 4, 1};
    memcpy(in, ip, 4);
    size_t n = dns_query(in + 4, "connectivitycheck.gstatic.com", 1, false);
    seed1("dns_a", 0, in, 4 + n);
    n = dns_query(in + 4, "captive.apple.com", 28, true);
    seed1("dns_aaaa_edns", 0, in, 4 + n);
    n = dns_query(in + 4, "www.msftconnecttest.com", 65, true);
    seed1("dns_https", 0, in, 4 + n);
    const char probe[] = "/generate_204\0connectivitycheck.gstatic.com\0192.168.4.1\0gw01";
    seed1("http_probe", 1u | (6u << 2), probe, sizeof(probe) - 1);
    const char page[] = "/portal?x\0gw01.local:80\0192.168.4.1\0gw01";
    seed1("http_page", 1u | (6u << 2), page, sizeof(page) - 1);
    const char tpl[] = "GW<01>\0GW-Setup-A1B2C3\0<h1>{{node}}</h1>{{#https}}s{{/https}}"
                       "{{^https}}p{{/https}}{{#claim}}{{ssid}}{{#mdns}}{{mdns}}{{/mdns}}"
                       "{{/claim}}{{url}}";
    seed1("page", 2u | (7u << 2), tpl, sizeof(tpl) - 1);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: fuzz_seeds <dir>\n");
        return 2;
    }
    s_dir = argv[1];
    mkdir_ok(s_dir);
    load_docs();
    seeds_mb_rtu();
    seeds_mbap();
    seeds_spb();
    seeds_node();
    seeds_config("fuzz_config");
    seeds_config("fuzz_cfgdoc");
    seeds_web_json();
    seeds_web_text();
    seeds_flash();
    seeds_nvs();
    seeds_text();
    seeds_sfspill();
    seeds_captive();
    printf("fuzz_seeds: %u seeds in %s\n", s_count, s_dir);
    return 0;
}
