/* #205 #221: the rules of the certificate files (gw_certs.h): the file names of the saved
 * configuration, the private key block of a text with a certificate and its key, the plan of a
 * load (the kind that a configured key needs, the pair check) and the hostname in the SANs. */
#include <stdio.h>
#include <string.h>

#include "gw_certs.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static const char *CERT =
    "-----BEGIN CERTIFICATE-----\nMIIBcert\n-----END CERTIFICATE-----\n";
static const char *KEY =
    "-----BEGIN EC PRIVATE KEY-----\nMHcCkey\n-----END EC PRIVATE KEY-----\n";

static void test_refs_parse(void)
{
    const char *doc = "{\"mqtt\":{\"host\":\"b\",\"tls\":{\"enabled\":true,\"ca_file\":\"ca.pem\","
                      "\"cert_file\":\"gw01.crt\",\"key_file\":\"gw01.key\"}},"
                      "\"ota\":{\"ca_file\":\"ca.pem\"}}";
    gw_filerefs_t r;
    TEST_ASSERT_TRUE(gw_filerefs_parse(doc, strlen(doc), &r));
    TEST_ASSERT_EQUAL_STRING("ca.pem", r.name[GW_FILEREF_MQTT_CA]);
    TEST_ASSERT_EQUAL_STRING("gw01.crt", r.name[GW_FILEREF_MQTT_CERT]);
    TEST_ASSERT_EQUAL_STRING("gw01.key", r.name[GW_FILEREF_MQTT_KEY]);
    TEST_ASSERT_EQUAL_STRING("ca.pem", r.name[GW_FILEREF_OTA_CA]);
    TEST_ASSERT_EQUAL_HEX(1u << GW_FILEREF_MQTT_CA | 1u << GW_FILEREF_OTA_CA,
                          gw_filerefs_users(&r, "ca.pem"));
    TEST_ASSERT_EQUAL_HEX(1u << GW_FILEREF_MQTT_KEY, gw_filerefs_users(&r, "gw01.key"));
    TEST_ASSERT_EQUAL_HEX(0, gw_filerefs_users(&r, "old.pem"));
    TEST_ASSERT_EQUAL_HEX(0, gw_filerefs_users(&r, ""));
    TEST_ASSERT_EQUAL_STRING("mqtt.tls.key_file", gw_fileref_path(GW_FILEREF_MQTT_KEY));
    TEST_ASSERT_EQUAL(GW_PEM_KEY, gw_fileref_kind(GW_FILEREF_MQTT_KEY));
    TEST_ASSERT_EQUAL(GW_PEM_CERTS, gw_fileref_kind(GW_FILEREF_OTA_CA));
}

/* A document that the firmware rejects still names its files; no "tls" or a number is absent;
 * a text that is not a JSON object gives false. */
static void test_refs_partial(void)
{
    gw_filerefs_t r;
    const char *bad = "{\"mqtt\":{\"tls\":{\"ca_file\":7,\"key_file\":\"k.pem\"}},\"devices\":5}";
    TEST_ASSERT_TRUE(gw_filerefs_parse(bad, strlen(bad), &r));
    TEST_ASSERT_EQUAL_STRING("", r.name[GW_FILEREF_MQTT_CA]);
    TEST_ASSERT_EQUAL_STRING("k.pem", r.name[GW_FILEREF_MQTT_KEY]);
    TEST_ASSERT_EQUAL_STRING("", r.name[GW_FILEREF_OTA_CA]);
    const char *none = "{\"mqtt\":{\"host\":\"b\"}}";
    TEST_ASSERT_TRUE(gw_filerefs_parse(none, strlen(none), &r));
    TEST_ASSERT_EQUAL_HEX(0, gw_filerefs_users(&r, "ca.pem"));
    TEST_ASSERT_FALSE(gw_filerefs_parse("[1]", 3, &r));
    TEST_ASSERT_FALSE(gw_filerefs_parse("{\"mqtt\":", 8, &r));
    char longname[80];
    snprintf(longname, sizeof(longname), "{\"ota\":{\"ca_file\":\"%040d\"}}", 1);
    TEST_ASSERT_TRUE(gw_filerefs_parse(longname, strlen(longname), &r));
    TEST_ASSERT_EQUAL_STRING("", r.name[GW_FILEREF_OTA_CA]);
}

static void test_refs_from_config(void)
{
    static gw_config_t c;
    memset(&c, 0, sizeof(c));
    strcpy(c.mqtt.tls.ca_file, "ca.pem");
    strcpy(c.ota.ca_file, "fw-ca.pem");
    gw_filerefs_t r;
    gw_filerefs_from_config(&c, &r);
    TEST_ASSERT_EQUAL_HEX(1u << GW_FILEREF_MQTT_CA, gw_filerefs_users(&r, "ca.pem"));
    TEST_ASSERT_EQUAL_HEX(1u << GW_FILEREF_OTA_CA, gw_filerefs_users(&r, "fw-ca.pem"));
    TEST_ASSERT_EQUAL_STRING("", r.name[GW_FILEREF_MQTT_CERT]);
}

static void test_key_blocks(void)
{
    char text[512];
    size_t off, n;
    snprintf(text, sizeof(text), "%s%s", CERT, KEY);
    TEST_ASSERT_EQUAL(1, gw_pem_key_blocks(text, strlen(text), &off, &n));
    TEST_ASSERT_EQUAL(strlen(CERT), off);
    TEST_ASSERT_EQUAL(strlen(KEY), n);
    /* the key first, a chain of two after it, CRLF line ends */
    const char *crlf = "-----BEGIN PRIVATE KEY-----\r\nMIGH\r\n-----END PRIVATE KEY-----\r\n";
    snprintf(text, sizeof(text), "%s%s%s", crlf, CERT, CERT);
    TEST_ASSERT_EQUAL(1, gw_pem_key_blocks(text, strlen(text), &off, &n));
    TEST_ASSERT_EQUAL(0, off);
    TEST_ASSERT_EQUAL(strlen(crlf), n);
    /* no key, two keys, an RSA key without a final new line */
    TEST_ASSERT_EQUAL(0, gw_pem_key_blocks(CERT, strlen(CERT), &off, &n));
    snprintf(text, sizeof(text), "%s%s", KEY, KEY);
    TEST_ASSERT_EQUAL(2, gw_pem_key_blocks(text, strlen(text), &off, &n));
    const char *rsa = "-----BEGIN RSA PRIVATE KEY-----\nMII\n-----END RSA PRIVATE KEY-----";
    TEST_ASSERT_EQUAL(1, gw_pem_key_blocks(rsa, strlen(rsa), &off, &n));
    TEST_ASSERT_EQUAL(strlen(rsa), n);
    /* a block without its END line is not a block */
    const char *cut = "-----BEGIN PRIVATE KEY-----\nMIGH";
    TEST_ASSERT_EQUAL(0, gw_pem_key_blocks(cut, strlen(cut), &off, &n));
    TEST_ASSERT_EQUAL(0, gw_pem_key_blocks("", 0, &off, &n));
}

static gw_filerefs_t refs_mtls(void)
{
    const char *doc = "{\"mqtt\":{\"tls\":{\"ca_file\":\"ca.pem\",\"cert_file\":\"gw01.crt\","
                      "\"key_file\":\"gw01.key\"}},\"ota\":{\"ca_file\":\"fw.pem\"}}";
    gw_filerefs_t r;
    gw_filerefs_parse(doc, strlen(doc), &r);
    return r;
}

static void test_plan_new_and_kind(void)
{
    gw_filerefs_t r = refs_mtls();
    gw_pemload_plan_t p;
    char path[40], why[200];
    /* a new file that nothing uses: no check */
    gw_pemload_file_t f = {"new.pem", GW_PEM_CERTS};
    TEST_ASSERT_TRUE(gw_pemload_plan(&r, &f, 1, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_FALSE(p.pair);
    /* a key in place of the CA file: refused at mqtt.tls.ca_file */
    f = (gw_pemload_file_t){"ca.pem", GW_PEM_KEY};
    TEST_ASSERT_FALSE(gw_pemload_plan(&r, &f, 1, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("mqtt.tls.ca_file", path);
    TEST_ASSERT_NOT_NULL(strstr(why, "must be PEM certificates"));
    /* certificates in place of the client key */
    f = (gw_pemload_file_t){"gw01.key", GW_PEM_CERTS};
    TEST_ASSERT_FALSE(gw_pemload_plan(&r, &f, 1, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("mqtt.tls.key_file", path);
    TEST_ASSERT_NOT_NULL(strstr(why, "unencrypted PEM private key"));
    /* the CA of the firmware updates replaced with a CA: no pair check */
    f = (gw_pemload_file_t){"fw.pem", GW_PEM_CERTS};
    TEST_ASSERT_TRUE(gw_pemload_plan(&r, &f, 1, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_FALSE(p.pair);
    TEST_ASSERT_EQUAL_STRING("", path);
}

static void test_plan_pair(void)
{
    gw_filerefs_t r = refs_mtls();
    gw_pemload_plan_t p;
    char path[40], why[200];
    /* only the client certificate: it must agree with the stored key */
    gw_pemload_file_t f = {"gw01.crt", GW_PEM_CERTS};
    TEST_ASSERT_TRUE(gw_pemload_plan(&r, &f, 1, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_TRUE(p.pair && p.configured && p.cert_new && !p.key_new);
    TEST_ASSERT_EQUAL_STRING("gw01.crt", p.cert);
    TEST_ASSERT_EQUAL_STRING("gw01.key", p.key);
    /* only the key: it must agree with the stored certificate */
    f = (gw_pemload_file_t){"gw01.key", GW_PEM_KEY};
    TEST_ASSERT_TRUE(gw_pemload_plan(&r, &f, 1, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_TRUE(p.pair && p.configured && !p.cert_new && p.key_new);
    /* the two together */
    gw_pemload_file_t two[2] = {{"gw01.crt", GW_PEM_CERTS}, {"gw01.key", GW_PEM_KEY}};
    TEST_ASSERT_TRUE(gw_pemload_plan(&r, two, 2, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_TRUE(p.pair && p.configured && p.cert_new && p.key_new);
    /* the configured certificate with a key of another name: the stored gw01.key stays the
     * key of the configuration, so the check is against it */
    gw_pemload_file_t other[2] = {{"gw01.crt", GW_PEM_CERTS}, {"gw01b.key", GW_PEM_KEY}};
    TEST_ASSERT_TRUE(gw_pemload_plan(&r, other, 2, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_TRUE(p.pair && p.configured && p.cert_new && !p.key_new);
    TEST_ASSERT_EQUAL_STRING("gw01.key", p.key);
    /* a new pair that the configuration does not use: the pair of the load */
    gw_pemload_file_t fresh[2] = {{"gw02.crt", GW_PEM_CERTS}, {"gw02.key", GW_PEM_KEY}};
    TEST_ASSERT_TRUE(gw_pemload_plan(&r, fresh, 2, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_TRUE(p.pair && !p.configured && p.cert_new && p.key_new);
    TEST_ASSERT_EQUAL_STRING("gw02.crt", p.cert);
    TEST_ASSERT_EQUAL_STRING("gw02.key", p.key);
    /* no mutual TLS in the configuration: a load of a key alone has no pair check */
    gw_filerefs_t none;
    memset(&none, 0, sizeof(none));
    f = (gw_pemload_file_t){"gw01.key", GW_PEM_KEY};
    TEST_ASSERT_TRUE(gw_pemload_plan(&none, &f, 1, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_FALSE(p.pair);
}

static void test_plan_refused_requests(void)
{
    gw_filerefs_t r = refs_mtls();
    gw_pemload_plan_t p;
    char path[40], why[200];
    gw_pemload_file_t same[2] = {{"a.pem", GW_PEM_CERTS}, {"a.pem", GW_PEM_KEY}};
    TEST_ASSERT_FALSE(gw_pemload_plan(&r, same, 2, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("", path);
    TEST_ASSERT_NOT_NULL(strstr(why, "two names"));
    gw_pemload_file_t kinds[2] = {{"a.pem", GW_PEM_CERTS}, {"b.pem", GW_PEM_CERTS}};
    TEST_ASSERT_FALSE(gw_pemload_plan(&r, kinds, 2, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "unencrypted private key"));
    TEST_ASSERT_FALSE(gw_pemload_plan(&r, kinds, 0, &p, path, sizeof(path), why, sizeof(why)));
    TEST_ASSERT_FALSE(gw_pemload_plan(&r, kinds, 3, &p, path, sizeof(path), why, sizeof(why)));
}

static void test_san_host(void)
{
    TEST_ASSERT_TRUE(gw_san_has_host("DNS:gw01.local,DNS:gw01", "gw01"));
    TEST_ASSERT_TRUE(gw_san_has_host("DNS:GW01.plant.local,IP:192.168.1.50", "gw01"));
    TEST_ASSERT_TRUE(gw_san_has_host("IP:10.0.0.1,DNS:gw-line3.local", "gw-line3"));
    TEST_ASSERT_FALSE(gw_san_has_host("DNS:gw01.local,DNS:gw01", "gw-line3"));
    TEST_ASSERT_FALSE(gw_san_has_host("DNS:gw011.local", "gw01"));
    TEST_ASSERT_FALSE(gw_san_has_host("DNS:gw01.", "gw01"));
    TEST_ASSERT_FALSE(gw_san_has_host("IP:192.168.1.50", "gw01"));
    TEST_ASSERT_FALSE(gw_san_has_host("DNS:*.plant.local", "gw01"));
    TEST_ASSERT_FALSE(gw_san_has_host("", "gw01"));
    TEST_ASSERT_FALSE(gw_san_has_host("DNS:gw01", ""));
}

/* #205: the NVS space of the certificate that waits for the restart */
static void test_nvs_need(void)
{
    /* a header entry, then the text with its NUL in entries of 32 bytes */
    TEST_ASSERT_EQUAL_UINT(2, gw_nvs_str_entries(0));
    TEST_ASSERT_EQUAL_UINT(2, gw_nvs_str_entries(31));
    TEST_ASSERT_EQUAL_UINT(3, gw_nvs_str_entries(32));
    /* the longest NVS string (4000 bytes with the NUL) fills one page */
    TEST_ASSERT_EQUAL_UINT(GW_NVS_PAGE_ENTRIES, gw_nvs_str_entries(3999));
    /* the strings, the copy of the certificate at the next boot, and one page: an EC certificate
     * of 700 bytes (23 entries) and its key of 250 bytes (9) in place of the same */
    TEST_ASSERT_EQUAL_UINT((23 + 9 + 2) + 23 + GW_NVS_PAGE_ENTRIES,
                           gw_webcert_nvs_need(700, 250, 6, 700));
    /* a key larger than the old certificate: its part above the erased certificate also */
    TEST_ASSERT_EQUAL_UINT((23 + 105 + 2) + 23 + (105 - 23) + GW_NVS_PAGE_ENTRIES,
                           gw_webcert_nvs_need(700, 3300, 6, 700));
    /* no certificate in use yet: the full key */
    TEST_ASSERT_EQUAL_UINT((23 + 9 + 2) + 23 + 9 + GW_NVS_PAGE_ENTRIES,
                           gw_webcert_nvs_need(700, 250, 6, 0));
    /* 5 pages of 126 entries for data (the sixth stays free), 59 entries in use in QEMU with the
     * certificate of the first boot (E2E https): a chain of 3.5 KB (111) with an RSA 2048 key (55)
     * fits, also as the renewal of the same; 3999 bytes with a key of 3999 bytes does not */
    const size_t data = 5 * GW_NVS_PAGE_ENTRIES, first = 59, plant = 111 + 55 + 2;
    TEST_ASSERT_TRUE(gw_webcert_nvs_need(3500, 1700, 6, 700) <= data - first);
    TEST_ASSERT_TRUE(gw_webcert_nvs_need(3500, 1700, 6, 3500) <= data - first - plant + 34);
    TEST_ASSERT_TRUE(gw_webcert_nvs_need(3999, 3999, 6, 700) > data - first);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_refs_parse);
    RUN_TEST(test_refs_partial);
    RUN_TEST(test_refs_from_config);
    RUN_TEST(test_key_blocks);
    RUN_TEST(test_plan_new_and_kind);
    RUN_TEST(test_plan_pair);
    RUN_TEST(test_plan_refused_requests);
    RUN_TEST(test_san_host);
    RUN_TEST(test_nvs_need);
    return UNITY_END();
}
