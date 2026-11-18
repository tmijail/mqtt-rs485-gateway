/* CFG-3 / FW-1 (issue #144 item 8): the network of the last valid configuration, in a format
 * that an earlier or a later image reads. */
#include <stdlib.h>
#include <string.h>

#include "gw_lkgnet.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static gw_config_t *a, *b;

static void fill(gw_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->network.hostname, "gw-plant-7");
    c->network.mdns = true;
    c->network.dhcp = false;
    const uint8_t ip[4] = {192, 168, 1, 50}, nm[4] = {255, 255, 255, 0}, gw[4] = {192, 168, 1, 1};
    memcpy(c->network.ip, ip, 4);
    memcpy(c->network.netmask, nm, 4);
    memcpy(c->network.gateway, gw, 4);
    memcpy(c->network.dns, gw, 4);
    c->network.ipv6.enabled = true;
    c->network.ipv6.slaac = true;
    c->network.ipv6.has_address = true;
    c->network.ipv6.address[0] = 0xfd;
    c->network.ipv6.address[15] = 0x07;
    c->network.wifi.enabled = true;
    strcpy(c->network.wifi.ssid, "plant");
    strcpy(c->network.wifi.password, "secret-psk-1");
    c->network.wifi.role = GW_WIFI_PRIMARY;
    c->network.wifi.failback_s = 45;
    c->network.wifi.ipv4.dhcp = true;
    c->web.port = 8080;
    c->web.https_port = 8443;
    c->web.https = true;
}

static void test_round_trip(void)
{
    fill(a);
    uint8_t buf[GW_LKG_MAX];
    size_t n = gw_lkg_encode(a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 5);
    TEST_ASSERT_EQUAL_MEMORY("GWLN", buf, 4);
    TEST_ASSERT_EQUAL_UINT8(GW_LKG_VERSION, buf[4]);
    memset(b, 0, sizeof(*b));
    TEST_ASSERT_TRUE(gw_lkg_decode(buf, n, b));
    TEST_ASSERT_EQUAL_MEMORY(&a->network, &b->network, sizeof(a->network));
    TEST_ASSERT_EQUAL_UINT16(8080, b->web.port);
    TEST_ASSERT_EQUAL_UINT16(8443, b->web.https_port);
    TEST_ASSERT_TRUE(b->web.https);
    /* without IPv6 address and DNS: those stay as they were (not present) */
    a->network.ipv6.has_address = false;
    memset(a->network.ipv6.address, 0, 16);
    n = gw_lkg_encode(a, buf, sizeof(buf));
    memset(b, 0, sizeof(*b));
    TEST_ASSERT_TRUE(gw_lkg_decode(buf, n, b));
    TEST_ASSERT_FALSE(b->network.ipv6.has_address);
}

/* A string field without its terminator: the record keeps the first size - 1 bytes of the
 * field and reads nothing of the next field (the bound is the field, not 255). */
static void test_unterminated_field(void)
{
    fill(a);
    memset(a->network.hostname, 'h', sizeof(a->network.hostname));
    memset(a->network.wifi.ssid, 's', sizeof(a->network.wifi.ssid));
    uint8_t buf[GW_LKG_MAX];
    size_t n = gw_lkg_encode(a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 5);
    memset(b, 0, sizeof(*b));
    TEST_ASSERT_TRUE(gw_lkg_decode(buf, n, b));
    TEST_ASSERT_EQUAL_size_t(sizeof(b->network.hostname) - 1, strlen(b->network.hostname));
    TEST_ASSERT_EACH_EQUAL_CHAR('h', b->network.hostname, sizeof(b->network.hostname) - 1);
    TEST_ASSERT_EQUAL_size_t(sizeof(b->network.wifi.ssid) - 1, strlen(b->network.wifi.ssid));
}

/* the largest record set fits GW_LKG_MAX; a buffer too small gives 0 */
static void test_sizes(void)
{
    memset(a, 0, sizeof(*a));
    memset(a->network.hostname, 'h', sizeof(a->network.hostname) - 1);
    memset(a->network.wifi.ssid, 's', sizeof(a->network.wifi.ssid) - 1);
    memset(a->network.wifi.password, 'p', sizeof(a->network.wifi.password) - 1);
    a->network.ipv6.has_address = a->network.ipv6.has_dns = true;
    uint8_t buf[GW_LKG_MAX];
    size_t n = gw_lkg_encode(a, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 250 && n <= GW_LKG_MAX);
    TEST_ASSERT_EQUAL(0, gw_lkg_encode(a, buf, n - 1));
    TEST_ASSERT_EQUAL(0, gw_lkg_encode(a, buf, 4));
}

/* a later image added a field (unknown tag), or gave a known field another length: skipped */
static void test_unknown_and_resized_fields(void)
{
    fill(a);
    uint8_t buf[GW_LKG_MAX + 32];
    size_t n = gw_lkg_encode(a, buf, GW_LKG_MAX);
    const uint8_t extra[] = {200, 3, 1, 2, 3,      /* unknown tag */
                             4, 6, 9, 9, 9, 9, 9, 9,   /* T_IP with 6 bytes: skipped */
                             3, 1, 1};                  /* T_DHCP: true, after the rest */
    memcpy(buf + n, extra, sizeof(extra));
    memset(b, 0, sizeof(*b));
    TEST_ASSERT_TRUE(gw_lkg_decode(buf, n + sizeof(extra), b));
    TEST_ASSERT_EQUAL_UINT8(192, b->network.ip[0]);   /* the 4-byte value of before */
    TEST_ASSERT_TRUE(b->network.dhcp);
    TEST_ASSERT_EQUAL_STRING("gw-plant-7", b->network.hostname);
    /* a later format version is read the same way */
    buf[4] = 9;
    TEST_ASSERT_TRUE(gw_lkg_decode(buf, n, b));
}

/* a missing field keeps the default of the caller (DHCP); a damaged set changes nothing */
static void test_missing_and_damaged(void)
{
    memset(b, 0, sizeof(*b));
    b->network.dhcp = true;
    b->web.port = 80;
    const uint8_t only_host[] = {'G', 'W', 'L', 'N', 1, 1, 3, 'a', 'b', 'c'};
    TEST_ASSERT_TRUE(gw_lkg_decode(only_host, sizeof(only_host), b));
    TEST_ASSERT_EQUAL_STRING("abc", b->network.hostname);
    TEST_ASSERT_TRUE(b->network.dhcp);
    TEST_ASSERT_EQUAL_UINT16(80, b->web.port);

    fill(a);
    uint8_t buf[GW_LKG_MAX];
    size_t n = gw_lkg_encode(a, buf, sizeof(buf));
    memset(b, 0, sizeof(*b));
    TEST_ASSERT_FALSE(gw_lkg_decode(buf, n - 1, b));   /* the last record cut */
    TEST_ASSERT_EQUAL_STRING("", b->network.hostname);  /* nothing taken */
    TEST_ASSERT_FALSE(gw_lkg_decode(buf, 4, b));
    buf[0] = 'X';
    TEST_ASSERT_FALSE(gw_lkg_decode(buf, n, b));
    buf[0] = 'G';
    buf[4] = 0;
    TEST_ASSERT_FALSE(gw_lkg_decode(buf, n, b));
    /* the raw struct of the firmware before #144: not a record set */
    uint8_t old[64];
    memset(old, 0, sizeof(old));
    old[0] = 0x40;
    TEST_ASSERT_FALSE(gw_lkg_decode(old, sizeof(old), b));
    /* a string with a NUL inside, or too long for the field: skipped */
    const uint8_t badstr[] = {'G', 'W', 'L', 'N', 1, 1, 3, 'a', 0, 'c', 13, 0};
    strcpy(b->network.hostname, "keep");
    TEST_ASSERT_TRUE(gw_lkg_decode(badstr, sizeof(badstr), b));
    TEST_ASSERT_EQUAL_STRING("keep", b->network.hostname);
    TEST_ASSERT_EQUAL_STRING("", b->network.wifi.ssid);   /* an empty string is a value */
    /* a role this image does not know: skipped */
    const uint8_t role[] = {'G', 'W', 'L', 'N', 1, 15, 1, 7};
    b->network.wifi.role = GW_WIFI_BACKUP;
    TEST_ASSERT_TRUE(gw_lkg_decode(role, sizeof(role), b));
    TEST_ASSERT_EQUAL(GW_WIFI_BACKUP, b->network.wifi.role);
}

int main(void)
{
    a = malloc(sizeof(*a));
    b = malloc(sizeof(*b));
    UNITY_BEGIN();
    RUN_TEST(test_round_trip);
    RUN_TEST(test_sizes);
    RUN_TEST(test_unterminated_field);
    RUN_TEST(test_unknown_and_resized_fields);
    RUN_TEST(test_missing_and_damaged);
    int r = UNITY_END();
    free(a);
    free(b);
    return r;
}
