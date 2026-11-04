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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_update_splits_secrets);
    RUN_TEST(test_update_mask_absent_empty);
    RUN_TEST(test_file_mode);
    RUN_TEST(test_errors);
    RUN_TEST(test_export_masks);
    RUN_TEST(test_jsonfeed);
    return UNITY_END();
}
