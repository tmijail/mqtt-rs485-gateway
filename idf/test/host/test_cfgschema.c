/* FW-1 (issue #144 item 3): the "since" schema that a stored configuration needs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_cfgschema.h"
#include "gw_config.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    TEST_ASSERT_EQUAL((size_t)n, fread(buf, 1, (size_t)n, f));
    buf[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void test_path_match(void)
{
    TEST_ASSERT_TRUE(gw_schema_path_match("devices[].tags[].scan", "devices[0].tags[12].scan"));
    TEST_ASSERT_TRUE(gw_schema_path_match("network.dhcp", "network.dhcp"));
    TEST_ASSERT_FALSE(gw_schema_path_match("network.dhcp", "network.dhcpx"));
    TEST_ASSERT_FALSE(gw_schema_path_match("network.dhcp", "network.dhc"));
    TEST_ASSERT_FALSE(gw_schema_path_match("devices[].name", "devices[].name"));   /* no index */
    TEST_ASSERT_FALSE(gw_schema_path_match("devices[].name", "devices[a].name"));
    TEST_ASSERT_FALSE(gw_schema_path_match("devices[].name", "devices[1.name"));
    TEST_ASSERT_FALSE(gw_schema_path_match("devices[].tags", "devices[1].tags[0]"));
    TEST_ASSERT_TRUE(gw_schema_path_match("modbus_tcp.allowed_clients[]",
                                          "modbus_tcp.allowed_clients[3]"));
}

static void test_since_lookup(void)
{
    static const gw_since_t t[] = {{"devices[].tags[].scan", NULL, 2},
                                   {"devices[].tags[].scan", "slow", 3},
                                   {"sparkplug.naming", "umh", 4}};
    TEST_ASSERT_EQUAL_UINT16(2, gw_schema_since(t, 3, "devices[1].tags[2].scan", NULL));
    TEST_ASSERT_EQUAL_UINT16(3, gw_schema_since(t, 3, "devices[1].tags[2].scan", "slow"));
    TEST_ASSERT_EQUAL_UINT16(1, gw_schema_since(t, 3, "devices[1].tags[2].scan", "fast"));
    TEST_ASSERT_EQUAL_UINT16(1, gw_schema_since(t, 3, "sparkplug.naming", NULL));
    TEST_ASSERT_EQUAL_UINT16(4, gw_schema_since(t, 3, "sparkplug.naming", "umh"));
    TEST_ASSERT_EQUAL_UINT16(1, gw_schema_since(t, 3, "network", NULL));
    TEST_ASSERT_EQUAL_UINT16(1, gw_schema_since(NULL, 0, "network", NULL));
}

/* the report keeps the highest numbers (first seen among equal ones) and counts every path */
static void test_report(void)
{
    static const gw_since_t t[] = {{"a", NULL, 2}, {"b", NULL, 5}, {"c", NULL, 3},
                                   {"d", NULL, 5}, {"e", NULL, 2}, {"f", "x", 4}};
    gw_schema_report_t r;
    gw_schema_init(&r, t, 6);
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_EQUAL_UINT16(0, r.need);
    char why[200];
    TEST_ASSERT_TRUE(gw_schema_fits(&r, 0, why, sizeof(why)));   /* an empty document */
    const char *keys[] = {"z", "a", "b", "c", "d", "e"};
    for (size_t i = 0; i < 6; i++) {
        gw_schema_note(&r, keys[i], NULL);
    }
    gw_schema_note(&r, "f", "x");
    TEST_ASSERT_EQUAL_UINT16(5, r.need);
    TEST_ASSERT_EQUAL_UINT32(7, r.count);
    TEST_ASSERT_EQUAL_STRING("b", r.top[0].path);
    TEST_ASSERT_EQUAL_STRING("d", r.top[1].path);
    TEST_ASSERT_EQUAL_STRING("f=x", r.top[2].path);
    TEST_ASSERT_EQUAL_UINT32(6, gw_schema_newer(&r, 1));
    TEST_ASSERT_EQUAL_UINT32(2, gw_schema_newer(&r, 4));
    TEST_ASSERT_EQUAL_UINT32(0, gw_schema_newer(&r, 5));
    TEST_ASSERT_TRUE(gw_schema_fits(&r, 5, why, sizeof(why)));
    TEST_ASSERT_TRUE(gw_schema_fits(&r, 9, why, sizeof(why)));
    TEST_ASSERT_FALSE(gw_schema_fits(&r, 4, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the configuration needs schema 5, the image reads 4: b (5), d (5)",
                             why);
    TEST_ASSERT_FALSE(gw_schema_fits(&r, 1, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the configuration needs schema 5, the image reads 1: b (5), d (5), "
                             "f=x (4) and 3 more", why);
    /* a long path is cut, and a small buffer is never overrun */
    char longp[100];
    memset(longp, 'p', sizeof(longp) - 1);
    longp[sizeof(longp) - 1] = '\0';
    static const gw_since_t lt[] = {{"", NULL, 0}};
    gw_schema_init(&r, lt, 0);
    gw_schema_note(&r, longp, NULL);
    TEST_ASSERT_EQUAL(GW_SCHEMA_PATH - 1, strlen(r.top[0].path));
    char small[16];
    TEST_ASSERT_FALSE(gw_schema_fits(&r, 0, small, sizeof(small)));
    TEST_ASSERT_EQUAL(sizeof(small) - 1, strlen(small));
}

/* the validator gives every key and accepted enum value of a whole document */
static void test_validator_notes_the_document(void)
{
    size_t len;
    char *json = read_file(GW_SPEC_EXAMPLE, &len);
    gw_config_t *cfg = malloc(sizeof(*cfg));
    gw_cfg_err_t err;
    gw_schema_report_t r;
    gw_config_schema_init(&r);
    TEST_ASSERT_TRUE_MESSAGE(gw_config_parse_schema(json, len, cfg, &err, &r), err.msg);
    TEST_ASSERT_TRUE(r.valid);
    /* #201: ntp.servers is the key of schema 2 in the example; all the others are schema 1 */
    TEST_ASSERT_EQUAL_UINT16(GW_CONFIG_SCHEMA, r.need);
    TEST_ASSERT_TRUE(r.count > 50);
    TEST_ASSERT_EQUAL_STRING("ntp.servers", r.top[0].path);   /* the highest number first */
    TEST_ASSERT_EQUAL_STRING("network", r.top[1].path);       /* then the first key */
    char why[200];
    TEST_ASSERT_TRUE(gw_schema_fits(&r, GW_CONFIG_SCHEMA, why, sizeof(why)));
    /* an image that read only an older schema would get the first paths */
    TEST_ASSERT_FALSE(gw_schema_fits(&r, 0, why, sizeof(why)));
    const char *want = "the configuration needs schema 2, the image reads 0: ntp.servers (2), "
                       "network (1), ";
    TEST_ASSERT_EQUAL_STRING_LEN(want, why, strlen(want));
    /* a rejected document: not valid */
    gw_config_schema_init(&r);
    const char *bad = "{\"network\": {}, \"oops\": 1}";
    TEST_ASSERT_FALSE(gw_config_parse_schema(bad, strlen(bad), cfg, &err, &r));
    TEST_ASSERT_FALSE(r.valid);
    /* the plain parse notes nothing */
    TEST_ASSERT_TRUE(gw_config_parse(json, len, cfg, &err));
    free(cfg);
    free(json);
}

/* the table of this firmware: no entry above GW_CONFIG_SCHEMA, none at 1 or less */
static void test_firmware_table(void)
{
    size_t n;
    const gw_since_t *t = gw_config_since_table(&n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(t[i].since > 1 && t[i].since <= GW_CONFIG_SCHEMA, t[i].path);
        TEST_ASSERT_TRUE(t[i].path != NULL && t[i].path[0] != '\0');
    }
    TEST_ASSERT_TRUE(GW_CONFIG_SCHEMA >= 1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_path_match);
    RUN_TEST(test_since_lookup);
    RUN_TEST(test_report);
    RUN_TEST(test_validator_notes_the_document);
    RUN_TEST(test_firmware_table);
    return UNITY_END();
}
