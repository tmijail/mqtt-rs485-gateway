/* QA-1: configuration documents with secrets (CFG-4 export/import, CFG-7 secret store). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "gw_cfgdoc.h"
#include "support.h"
#include "unity.h"

static gw_config_t cfg;
static gw_cfg_err_t err;
static gw_secrets_t stored;
static gw_cfgdoc_t doc;

void setUp(void)
{
    memset(&stored, 0, sizeof(stored));
}

void tearDown(void)
{
    gw_cfgdoc_free(&doc);
}

#define BASE                                                                                 \
    "\"mqtt\":{\"host\":\"b\"%s},\"sparkplug\":{\"group_id\":\"G\",\"edge_node_id\":\"E\"}"

static const char *docf(const char *mqtt_extra, const char *rest)
{
    static char buf[1024];
    char base[256];
    snprintf(base, sizeof(base), BASE, mqtt_extra);
    snprintf(buf, sizeof(buf), "{%s%s}", base, rest);
    return buf;
}

static bool apply(const char *json, gw_cfgdoc_mode_t mode)
{
    gw_cfgdoc_free(&doc);
    return gw_cfgdoc_apply(json, strlen(json), mode, &stored, &cfg, &doc, &err);
}

/* Value of @p path ("a.b.c") in a JSON text, or NULL if absent. Static result. */
static const char *json_str(const char *text, const char *path)
{
    static char out[128];
    cJSON *root = cJSON_Parse(text);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, text);
    cJSON *o = root;
    char p[64];
    strcpy(p, path);
    for (char *tok = strtok(p, "."); tok && o; tok = strtok(NULL, ".")) {
        o = cJSON_GetObjectItemCaseSensitive(o, tok);
    }
    const char *r = NULL;
    if (cJSON_IsString(o)) {
        snprintf(out, sizeof(out), "%s", o->valuestring);
        r = out;
    } else if (o != NULL) {
        r = "<non-string>";
    }
    cJSON_Delete(root);
    return r;
}

/* PUT with new secrets: the document comes back without them, the secrets apart. */
static void test_update_splits_secrets(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        apply(docf(",\"password\":\"mq-secret\"",
                   ",\"network\":{\"wifi\":{\"enabled\":true,\"ssid\":\"S\",\"password\":"
                   "\"wifi-pass\"}},\"web\":{\"password\":\"web-pass-1\"}"),
              GW_CFGDOC_UPDATE),
        err.msg);
    TEST_ASSERT_EQUAL_STRING("mq-secret", cfg.mqtt.password);
    TEST_ASSERT_EQUAL_STRING("wifi-pass", cfg.network.wifi.password);
    TEST_ASSERT_EQUAL_STRING("web-pass-1", cfg.web.password);
    TEST_ASSERT_TRUE(doc.new_web_password);
    TEST_ASSERT_TRUE(doc.secrets_changed);
    TEST_ASSERT_EQUAL_STRING("mq-secret", doc.secrets.mqtt_password);
    TEST_ASSERT_EQUAL_STRING("wifi-pass", doc.secrets.wifi_password);
    TEST_ASSERT_NULL(strstr(doc.doc, "secret"));
    TEST_ASSERT_NULL(strstr(doc.doc, "pass"));
    TEST_ASSERT_NULL(json_str(doc.doc, "mqtt.password"));
    TEST_ASSERT_EQUAL_STRING("S", json_str(doc.doc, "network.wifi.ssid"));
    TEST_ASSERT_EQUAL(strlen(doc.doc), doc.doc_len);
    TEST_ASSERT_NOT_NULL(strstr(doc.doc, "\n  \"mqtt\": {\n    \"host\": \"b\""));   /* 2 spaces */
    TEST_ASSERT_NULL(strchr(doc.doc, '\t'));
    /* the stored document is itself valid and gives the same configuration minus secrets */
    gw_config_t again;
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse(doc.doc, doc.doc_len, &again, &err), err.msg);
    TEST_ASSERT_EQUAL_STRING("", again.mqtt.password);
}

/* GW_SECRET_MASK keeps the stored secret; absent clears it; "" clears it. */
static void test_update_mask_absent_empty(void)
{
    strcpy(stored.mqtt_password, "old-mq");
    strcpy(stored.wifi_password, "old-wifi-pw");
    strcpy(stored.web_password_hash, "pbkdf2-sha256$1$00$00");
    TEST_ASSERT_TRUE_MESSAGE(
        apply(docf(",\"password\":\"" GW_SECRET_MASK "\"",
                   ",\"network\":{\"wifi\":{\"password\":\"" GW_SECRET_MASK "\"}},"
                   "\"web\":{\"password\":\"" GW_SECRET_MASK "\"}"),
              GW_CFGDOC_UPDATE),
        err.msg);
    TEST_ASSERT_EQUAL_STRING("old-mq", cfg.mqtt.password);
    TEST_ASSERT_EQUAL_STRING("old-wifi-pw", cfg.network.wifi.password);
    TEST_ASSERT_EQUAL_STRING("", cfg.web.password);
    TEST_ASSERT_EQUAL_STRING(stored.web_password_hash, cfg.web.password_hash);
    TEST_ASSERT_FALSE(doc.secrets_changed);
    TEST_ASSERT_FALSE(doc.new_web_password);
    TEST_ASSERT_NULL(strstr(doc.doc, GW_SECRET_MASK));

    TEST_ASSERT_TRUE_MESSAGE(apply(docf(",\"password\":\"\"", ""), GW_CFGDOC_UPDATE), err.msg);
    TEST_ASSERT_EQUAL_STRING("", cfg.mqtt.password);
    TEST_ASSERT_EQUAL_STRING("", doc.secrets.wifi_password);      /* section absent */
    TEST_ASSERT_EQUAL_STRING("", doc.secrets.web_password_hash);  /* web absent */
    TEST_ASSERT_TRUE(doc.secrets_changed);
}

/* Boot: secrets come from the store unless the file carries them in clear (migration). */
static void test_file_mode(void)
{
    strcpy(stored.mqtt_password, "nvs-mq");
    strcpy(stored.web_password_hash, "pbkdf2-sha256$1$00$00");
    TEST_ASSERT_TRUE_MESSAGE(apply(docf("", ""), GW_CFGDOC_FILE), err.msg);
    TEST_ASSERT_EQUAL_STRING("nvs-mq", cfg.mqtt.password);
    TEST_ASSERT_EQUAL_STRING(stored.web_password_hash, cfg.web.password_hash);
    TEST_ASSERT_FALSE(doc.had_clear_secrets);
    TEST_ASSERT_FALSE(doc.secrets_changed);

    TEST_ASSERT_TRUE_MESSAGE(apply(docf(",\"password\":\"\"", ""), GW_CFGDOC_FILE), err.msg);
    TEST_ASSERT_EQUAL_STRING("nvs-mq", cfg.mqtt.password);   /* "" in the file = from the store */
    TEST_ASSERT_FALSE(doc.had_clear_secrets);

    TEST_ASSERT_TRUE_MESSAGE(
        apply(docf(",\"password\":\"clear-mq\"", ",\"web\":{\"password\":\"clear-web\"}"),
              GW_CFGDOC_FILE),
        err.msg);
    TEST_ASSERT_TRUE(doc.had_clear_secrets);
    TEST_ASSERT_TRUE(doc.secrets_changed);
    TEST_ASSERT_TRUE(doc.new_web_password);
    TEST_ASSERT_EQUAL_STRING("clear-mq", cfg.mqtt.password);
    TEST_ASSERT_EQUAL_STRING("clear-web", cfg.web.password);
    TEST_ASSERT_NULL(strstr(doc.doc, "clear"));
}

/* Validation errors keep their JSON path, also for secrets; nothing is allocated. */
static void test_errors(void)
{
    TEST_ASSERT_FALSE(apply(docf(",\"password\":5", ""), GW_CFGDOC_UPDATE));
    TEST_ASSERT_EQUAL_STRING("mqtt.password", err.path);
    TEST_ASSERT_NULL(doc.doc);
    TEST_ASSERT_FALSE(apply(docf("", ",\"network\":{\"wifi\":{\"password\":\"short\"}}"),
                            GW_CFGDOC_UPDATE));
    TEST_ASSERT_EQUAL_STRING("network.wifi.password", err.path);
    TEST_ASSERT_FALSE(apply(docf("", ",\"web\":{\"password\":\"1234567\"}"), GW_CFGDOC_UPDATE));
    TEST_ASSERT_EQUAL_STRING("web.password", err.path);
    TEST_ASSERT_FALSE(apply("{\n\"mqtt\": {\"host\": \"b\"}\n\"x\":1}", GW_CFGDOC_UPDATE));
    TEST_ASSERT_EQUAL_STRING("", err.path);
    TEST_ASSERT_EQUAL(3, err.line);
    TEST_ASSERT_EQUAL(1, err.column);
    TEST_ASSERT_FALSE(apply(docf("", ",\"devices\":[{\"name\":\"D\",\"unit_id\":1,\"tags\":[{"
                                     "\"name\":\"t\",\"area\":\"coil\",\"address\":0,\"type\":"
                                     "\"int16\"}]}]"),
                            GW_CFGDOC_UPDATE));
    TEST_ASSERT_EQUAL_STRING("devices[0].tags[0].type", err.path);
}

static void test_export_masks(void)
{
    strcpy(stored.mqtt_password, "nvs-mq");
    strcpy(stored.web_password_hash, "pbkdf2-sha256$1$00$00");
    const char *in = docf(",\"password\":\"\"", ",\"network\":{\"wifi\":{\"ssid\":\"S\"}}");
    size_t n;
    char *out = gw_cfgdoc_export(in, strlen(in), &stored, &n, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(out, err.msg);
    TEST_ASSERT_EQUAL(strlen(out), n);
    TEST_ASSERT_EQUAL_STRING(GW_SECRET_MASK, json_str(out, "mqtt.password"));
    TEST_ASSERT_NULL(json_str(out, "network.wifi.password"));   /* not set: absent */
    TEST_ASSERT_EQUAL_STRING(GW_SECRET_MASK, json_str(out, "web.password"));   /* created */
    TEST_ASSERT_NULL(strstr(out, "nvs-mq"));
    TEST_ASSERT_NULL(strstr(out, "pbkdf2"));
    /* re-importing the export keeps every secret */
    TEST_ASSERT_TRUE_MESSAGE(apply(out, GW_CFGDOC_UPDATE), err.msg);
    TEST_ASSERT_FALSE(doc.secrets_changed);
    TEST_ASSERT_EQUAL_STRING("nvs-mq", cfg.mqtt.password);
    gw_cfgdoc_free_text(out);

    /* a secret in clear in a (not yet migrated) file is masked too */
    memset(&stored, 0, sizeof(stored));
    in = docf(",\"password\":\"clear\"", "");
    out = gw_cfgdoc_export(in, strlen(in), &stored, &n, &err);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_STRING(GW_SECRET_MASK, json_str(out, "mqtt.password"));
    TEST_ASSERT_NULL(strstr(out, "clear"));
    gw_cfgdoc_free_text(out);

    /* semantic errors do not stop an export (recovery); syntax errors do, with the position */
    in = "{\"mqtt\":{\"host\":\"\"},\"devices\":5}";
    out = gw_cfgdoc_export(in, strlen(in), &stored, &n, &err);
    TEST_ASSERT_NOT_NULL(out);
    gw_cfgdoc_free_text(out);
    in = "{\"mqtt\":{\"host\":\"b\"}\n,,}";
    TEST_ASSERT_NULL(gw_cfgdoc_export(in, strlen(in), &stored, &n, &err));
    TEST_ASSERT_EQUAL(2, err.line);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "JSON mal formado"));
    in = "[1]";
    TEST_ASSERT_NULL(gw_cfgdoc_export(in, strlen(in), &stored, &n, &err));
    TEST_ASSERT_EQUAL_STRING("$", err.path);
}

/* CFG-6: a document pasted on the console, in pieces of any size. */
/* The Modbus keys (MB-6, MB-11, MB-12, MB-15) survive export and re-import unchanged. */
static void test_modbus_keys_round_trip(void)
{
    const char *in = docf(",\"password\":\"mq-secret\"",
        ",\"modbus_tcp\":{\"enabled\":true,\"port\":1502,\"allowed_clients\":[\"10.0.0.0/8\"]},"
        "\"devices\":[{\"name\":\"M\",\"unit_id\":1,\"max_gap\":7,\"slow_poll_ms\":9000,"
        "\"tags\":[{\"name\":\"E\",\"area\":\"holding\",\"address\":0,\"type\":\"uint64\","
        "\"word_order\":\"DCBA\",\"scan\":\"slow\"},"
        "{\"name\":\"S\",\"area\":\"holding\",\"address\":4,\"type\":\"string\",\"length\":3},"
        "{\"name\":\"B\",\"area\":\"input\",\"address\":9,\"type\":\"bool\",\"bit\":12}]},"
        "{\"name\":\"N\",\"unit_id\":2,\"block_reads\":false,\"tags\":[{\"name\":\"T\","
        "\"area\":\"holding\",\"address\":0,\"type\":\"uint16\"}]}]");
    TEST_ASSERT_TRUE_MESSAGE(apply(in, GW_CFGDOC_UPDATE), err.msg);
    static gw_config_t first;
    first = cfg;
    stored = doc.secrets;
    size_t n;
    char *out = gw_cfgdoc_export(doc.doc, doc.doc_len, &stored, &n, &err);
    TEST_ASSERT_NOT_NULL_MESSAGE(out, err.msg);
    TEST_ASSERT_TRUE_MESSAGE(apply(out, GW_CFGDOC_UPDATE), err.msg);
    gw_cfgdoc_free_text(out);
    TEST_ASSERT_FALSE(doc.secrets_changed);
    TEST_ASSERT_TRUE(cfg.modbus_tcp.enabled);
    TEST_ASSERT_EQUAL(1502, cfg.modbus_tcp.port);
    TEST_ASSERT_EQUAL(8, cfg.modbus_tcp.clients[0].prefix);
    TEST_ASSERT_EQUAL(7, cfg.devices[0].max_gap);
    TEST_ASSERT_EQUAL_UINT32(9000, cfg.devices[0].slow_poll_ms);
    TEST_ASSERT_EQUAL(0, cfg.devices[1].block_reads);
    TEST_ASSERT_TRUE(cfg.tags[0].slow);
    TEST_ASSERT_EQUAL(GW_WORD_DCBA, cfg.tags[0].word_order);
    TEST_ASSERT_EQUAL(3, cfg.tags[1].length);
    TEST_ASSERT_EQUAL(12, cfg.tags[2].bit);
    TEST_ASSERT_EQUAL_MEMORY(&first.devices, &cfg.devices, sizeof(cfg.devices));
    TEST_ASSERT_EQUAL_MEMORY(&first.tags, &cfg.tags, sizeof(cfg.tags));
    TEST_ASSERT_EQUAL_MEMORY(&first.modbus_tcp, &cfg.modbus_tcp, sizeof(cfg.modbus_tcp));
}

static void test_jsonfeed(void)
{
    char buf[64];
    gw_jsonfeed_t f;
    gw_jsonfeed_init(&f, buf, sizeof(buf));
    TEST_ASSERT_FALSE(gw_jsonfeed_add(&f, "paste it now\n"));   /* before the document */
    TEST_ASSERT_FALSE(gw_jsonfeed_add(&f, "{\"a\": \"}{[\\\"\",\n"));
    TEST_ASSERT_FALSE(gw_jsonfeed_add(&f, " \"b\": [1, {\"c\": 2}]"));
    TEST_ASSERT_TRUE(gw_jsonfeed_add(&f, "}\ntrailing"));
    TEST_ASSERT_EQUAL_STRING("{\"a\": \"}{[\\\"\",\n \"b\": [1, {\"c\": 2}]}", buf);
    TEST_ASSERT_FALSE(f.overflow);
    gw_jsonfeed_init(&f, buf, 8);
    TEST_ASSERT_TRUE(gw_jsonfeed_add(&f, "{\"long\": 12345}"));
    TEST_ASSERT_TRUE(f.overflow);
    TEST_ASSERT_EQUAL(7, f.len);
}

/* ---------- issue #91: the document printer ---------- */

/* The printer before issue #91, as the reference for the text: cJSON_Print, then two spaces
 * per indentation tab and one space for the tab after ':'. */
static char *reference_print(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, json);
    /* no secret is stored: the export drops the empty secret keys */
    cJSON *net = cJSON_GetObjectItemCaseSensitive(root, "network");
    cJSON *holders[] = {cJSON_GetObjectItemCaseSensitive(root, "mqtt"),
                        cJSON_GetObjectItemCaseSensitive(root, "web"),
                        net != NULL ? cJSON_GetObjectItemCaseSensitive(net, "wifi") : NULL};
    for (size_t i = 0; i < sizeof(holders) / sizeof(holders[0]); i++) {
        if (holders[i] != NULL) {
            cJSON_DeleteItemFromObjectCaseSensitive(holders[i], "password");
        }
    }
    char *raw = cJSON_Print(root);
    cJSON_Delete(root);
    TEST_ASSERT_NOT_NULL(raw);
    char *out = malloc(2 * strlen(raw) + 2);
    size_t o = 0;
    bool line_start = true;
    for (const char *c = raw; *c != '\0'; c++) {
        if (*c == '\t') {
            if (line_start) {
                out[o++] = ' ';
            }
            out[o++] = ' ';
            continue;
        }
        line_start = *c == '\n';
        out[o++] = *c;
    }
    out[o++] = '\n';
    out[o] = '\0';
    free(raw);
    return out;
}

static size_t s_largest_request;
static unsigned s_requests_over_1k;

static void *tracking_alloc(size_t size)
{
    if (size > s_largest_request) {
        s_largest_request = size;
    }
    s_requests_over_1k += size > 1024;
    return malloc(size);
}

/* Export (no secrets set: nothing is masked) gives exactly the reference text, and the
 * largest block it asks for is the document itself, once. */
static void check_same_text(const char *json)
{
    char *ref = reference_print(json);
    s_largest_request = 0;
    s_requests_over_1k = 0;
    gw_config_set_allocator(tracking_alloc, free);
    size_t n = 0;
    char *out = gw_cfgdoc_export(json, strlen(json), &stored, &n, &err);
    gw_config_set_allocator(malloc, free);
    TEST_ASSERT_NOT_NULL_MESSAGE(out, err.msg);
    TEST_ASSERT_EQUAL_STRING(ref, out);
    TEST_ASSERT_EQUAL(strlen(ref), n);
    if (n + 1 > 256) {   /* smaller ones: a cJSON node can be the largest */
        TEST_ASSERT_EQUAL(n + 1, s_largest_request);
    }
    TEST_ASSERT_TRUE(n + 1 <= 1024 || s_requests_over_1k == 1);
    gw_cfgdoc_free_text(out);
    free(ref);
}

static void test_printer_matches_cjson(void)
{
    check_same_text("{\"a\":{},\"b\":[],\"c\":[[1,2],[{\"x\":{}},{}],[]],"
                    "\"n\":[0,-1,1.5,0.1,1e300,-2.5e-7,123456789012,9007199254740993,"
                    "3.14159265358979,-0.0,1e-320,2147483647,-2147483648,4294967296],"
                    "\"s\":\"q\\\"b\\\\s\\b\\f\\n\\r\\t\\u0001\\u001f \xc3\xa9/\",\"\":\"\","
                    "\"t\":true,\"f\":false,\"z\":null,"
                    "\"deep\":{\"x\":{\"y\":[{\"z\":[{\"w\":[1,{\"v\":\"\"}]}]}]}}}");
    check_same_text("{}");
    check_same_text("{\"only\":[{}]}");
    size_t len;
    const char *spec = support_read_file(GW_SPEC_EXAMPLE, &len);
    check_same_text(spec);
    const char *def = support_read_file(GW_DEFAULT_CONFIG, &len);
    check_same_text(def);
}

/* A validation (PUT) asks for no block larger than its document: before issue #91 the print
 * asked for about twice the document in one block. */
static void test_apply_largest_block_is_the_document(void)
{
    size_t len;
    const char *spec = support_read_file(GW_SPEC_EXAMPLE, &len);
    gw_cfgdoc_free(&doc);
    s_largest_request = 0;
    s_requests_over_1k = 0;
    gw_config_set_allocator(tracking_alloc, free);
    bool ok = gw_cfgdoc_apply(spec, len, GW_CFGDOC_UPDATE, &stored, &cfg, &doc, &err);
    gw_config_set_allocator(malloc, free);
    TEST_ASSERT_TRUE_MESSAGE(ok, err.msg);
    TEST_ASSERT_GREATER_THAN(1024, doc.doc_len);
    TEST_ASSERT_EQUAL(doc.doc_len + 1, s_largest_request);
    TEST_ASSERT_EQUAL(1, s_requests_over_1k);
    TEST_ASSERT_EQUAL(strlen(doc.doc), doc.doc_len);
    TEST_ASSERT_EQUAL('\n', doc.doc[doc.doc_len - 1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_update_splits_secrets);
    RUN_TEST(test_update_mask_absent_empty);
    RUN_TEST(test_file_mode);
    RUN_TEST(test_errors);
    RUN_TEST(test_export_masks);
    RUN_TEST(test_jsonfeed);
    RUN_TEST(test_modbus_keys_round_trip);
    RUN_TEST(test_printer_matches_cjson);
    RUN_TEST(test_apply_largest_block_is_the_document);
    return UNITY_END();
}
