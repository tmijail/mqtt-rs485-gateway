/* CFG-3 / NET-5, issue #120: the claim window (gw_claim.h). The cases of the issue: the window
 * opens, closes at the end of its time, closes after a set, refuses a short password and
 * refuses a set while closed; it never opens with a web password set; only a client of the
 * access point may use it. */
#include <stdio.h>
#include <string.h>

#include "gw_claim.h"
#include "unity.h"

static gw_claim_t c;

void setUp(void)
{
    gw_claim_init(&c);
}

void tearDown(void) {}

#define MIN10 (10u * 60u * 1000u)

/* The BOOT gesture opens the window for its time; at the end of the time it is closed. */
static void test_opens_and_closes_at_the_end_of_the_time(void)
{
    TEST_ASSERT_FALSE(gw_claim_active(&c, 0));
    TEST_ASSERT_EQUAL_UINT32(0, gw_claim_left_ms(&c, 0));
    TEST_ASSERT_TRUE(gw_claim_open(&c, false, MIN10, 5000));
    TEST_ASSERT_TRUE(gw_claim_active(&c, 5000));
    TEST_ASSERT_EQUAL_UINT32(MIN10, gw_claim_left_ms(&c, 5000));
    TEST_ASSERT_TRUE(gw_claim_active(&c, 5000 + MIN10 - 1));
    TEST_ASSERT_EQUAL_UINT32(1, gw_claim_left_ms(&c, 5000 + MIN10 - 1));
    TEST_ASSERT_FALSE(gw_claim_active(&c, 5000 + MIN10));
    TEST_ASSERT_FALSE(c.open);
    TEST_ASSERT_EQUAL_UINT32(0, gw_claim_left_ms(&c, 5000 + MIN10));
    /* a set after the time: refused */
    TEST_ASSERT_EQUAL(GW_CLAIM_CLOSED,
                      gw_claim_check(&c, false, true, "good-password", 5000 + MIN10 + 1));
    TEST_ASSERT_EQUAL_UINT32(1, c.opened);
}

/* A second gesture while the window is open starts its time again. */
static void test_second_gesture_starts_the_time_again(void)
{
    TEST_ASSERT_TRUE(gw_claim_open(&c, false, 60000, 0));
    TEST_ASSERT_TRUE(gw_claim_open(&c, false, 60000, 50000));
    TEST_ASSERT_TRUE(gw_claim_active(&c, 100000));
    TEST_ASSERT_FALSE(gw_claim_active(&c, 110000));
    TEST_ASSERT_EQUAL_UINT32(2, c.opened);
}

/* With a web password set the window never opens, and a set is refused. */
static void test_never_opens_with_a_password_set(void)
{
    TEST_ASSERT_FALSE(gw_claim_open(&c, true, MIN10, 0));
    TEST_ASSERT_FALSE(gw_claim_active(&c, 0));
    TEST_ASSERT_EQUAL(GW_CLAIM_PASSWORD_SET, gw_claim_check(&c, true, true, "good-password", 0));
    TEST_ASSERT_EQUAL_UINT32(0, c.opened);
    /* a password that appears while the window is open (not from the window) wins too */
    TEST_ASSERT_TRUE(gw_claim_open(&c, false, MIN10, 0));
    TEST_ASSERT_EQUAL(GW_CLAIM_PASSWORD_SET, gw_claim_check(&c, true, true, "good-password", 1));
    /* a zero time opens nothing */
    gw_claim_init(&c);
    TEST_ASSERT_FALSE(gw_claim_open(&c, false, 0, 0));
    TEST_ASSERT_FALSE(gw_claim_active(&c, 0));
}

/* A set is refused while the window is closed: before the gesture. */
static void test_set_refused_while_closed(void)
{
    TEST_ASSERT_EQUAL(GW_CLAIM_CLOSED, gw_claim_check(&c, false, true, "good-password", 0));
    TEST_ASSERT_EQUAL(GW_CLAIM_CLOSED, gw_claim_check(&c, false, false, "good-password", 0));
}

/* The first set closes the window: a second set is refused, and the window does not open
 * again (the password is set now). */
static void test_closes_after_a_set(void)
{
    TEST_ASSERT_TRUE(gw_claim_open(&c, false, MIN10, 0));
    TEST_ASSERT_EQUAL(GW_CLAIM_OK, gw_claim_check(&c, false, true, "good-password", 1000));
    TEST_ASSERT_TRUE(gw_claim_active(&c, 1000));   /* open until the password is stored */
    gw_claim_done(&c);
    TEST_ASSERT_FALSE(gw_claim_active(&c, 1001));
    TEST_ASSERT_EQUAL_UINT32(1, c.claimed);
    TEST_ASSERT_EQUAL(GW_CLAIM_PASSWORD_SET, gw_claim_check(&c, true, true, "other-password", 1002));
    TEST_ASSERT_FALSE(gw_claim_open(&c, true, MIN10, 2000));
}

/* Only a client of the access point: a client of another interface is refused while the window
 * is open, and the refusal does not close the window. */
static void test_only_from_the_access_point(void)
{
    TEST_ASSERT_TRUE(gw_claim_open(&c, false, MIN10, 0));
    TEST_ASSERT_EQUAL(GW_CLAIM_NOT_AP, gw_claim_check(&c, false, false, "good-password", 1));
    TEST_ASSERT_TRUE(gw_claim_active(&c, 2));
    TEST_ASSERT_EQUAL(GW_CLAIM_OK, gw_claim_check(&c, false, true, "good-password", 3));
}

/* The password rules (issue #187): the rule of a WPA2 passphrase, 8 to 63 characters, each one
 * printable ASCII (0x20 to 0x7E). A refused password keeps the window open. */
static void test_password_rules(void)
{
    TEST_ASSERT_TRUE(gw_claim_open(&c, false, MIN10, 0));
    TEST_ASSERT_EQUAL(GW_CLAIM_TOO_SHORT, gw_claim_check(&c, false, true, "", 1));
    TEST_ASSERT_EQUAL(GW_CLAIM_TOO_SHORT, gw_claim_check(&c, false, true, "1234567", 1));
    TEST_ASSERT_EQUAL(GW_CLAIM_OK, gw_claim_check(&c, false, true, "12345678", 1));
    TEST_ASSERT_EQUAL_INT(63, GW_CLAIM_PW_MAX);
    char lng[GW_CLAIM_PW_MAX + 2];
    memset(lng, 'a', sizeof(lng) - 1);
    lng[sizeof(lng) - 1] = '\0';
    TEST_ASSERT_EQUAL(GW_CLAIM_TOO_LONG, gw_claim_check(&c, false, true, lng, 1));
    lng[GW_CLAIM_PW_MAX] = '\0';
    TEST_ASSERT_EQUAL(GW_CLAIM_OK, gw_claim_check(&c, false, true, lng, 1));
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_CHAR, gw_claim_check(&c, false, true, "pass\tword1", 1));
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_CHAR, gw_claim_check(&c, false, true, "password\x7f", 1));
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_CHAR, gw_claim_check(&c, false, true, "password\xc3", 1));
    /* letters outside ASCII: refused (a WPA2 passphrase; phones convert them differently) */
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_CHAR, gw_claim_check(&c, false, true, "pass word:\xc3\xb1", 1));
    /* spaces, a colon and the symbols are allowed (HTTP Basic splits at the first colon: the
     * user name has none) */
    TEST_ASSERT_EQUAL(GW_CLAIM_OK, gw_claim_check(&c, false, true, "pass word: ~!@#$%^&*", 1));
    TEST_ASSERT_TRUE(gw_claim_active(&c, 1));
    TEST_ASSERT_EQUAL_UINT32(0, c.claimed);
}

/* The body of POST /api/claim. */
static void test_parse(void)
{
    char pw[GW_CLAIM_PW_MAX + 1];
#define P(s) gw_claim_parse(s, strlen(s), pw, sizeof(pw))
    TEST_ASSERT_EQUAL(GW_CLAIM_OK, P("{\"password\":\"good-password\"}"));
    TEST_ASSERT_EQUAL_STRING("good-password", pw);
    TEST_ASSERT_EQUAL(GW_CLAIM_OK, P("{\"password\":\"a\\u00f1\\\"b\"}"));
    TEST_ASSERT_EQUAL_STRING("a\xc3\xb1\"b", pw);
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_REQUEST, P(""));
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_REQUEST, P("not json"));
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_REQUEST, P("[\"password\"]"));
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_REQUEST, P("{}"));
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_REQUEST, P("{\"password\":12345678}"));
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_REQUEST, P("{\"password\":\"good-password\",\"user\":\"x\"}"));
    char big[200];
    snprintf(big, sizeof(big), "{\"password\":\"%0*d\"}", GW_CLAIM_PW_MAX + 1, 0);
    TEST_ASSERT_EQUAL(GW_CLAIM_TOO_LONG, P(big));
    snprintf(big, sizeof(big), "{\"password\":\"%0*d\"}", GW_CLAIM_PW_MAX, 0);
    TEST_ASSERT_EQUAL(GW_CLAIM_OK, P(big));
    TEST_ASSERT_EQUAL_size_t(GW_CLAIM_PW_MAX, strlen(pw));
    /* an embedded NUL would cut the password: refused */
    TEST_ASSERT_EQUAL(GW_CLAIM_BAD_CHAR, P("{\"password\":\"good\\u0000password\"}"));
#undef P
    for (int r = GW_CLAIM_OK; r <= GW_CLAIM_BAD_CHAR; r++) {
        TEST_ASSERT_NOT_NULL(gw_claim_text((gw_claim_result_t)r));
        TEST_ASSERT_TRUE(strlen(gw_claim_text((gw_claim_result_t)r)) > 5);
    }
}

static gw_claim_if_t ifc(uint8_t a, uint8_t b, uint8_t cc, uint8_t d, uint8_t bits)
{
    gw_claim_if_t i = {{a, b, cc, d}, {0, 0, 0, 0}};
    uint32_t m = bits == 0 ? 0 : 0xFFFFFFFFu << (32 - bits);
    i.mask[0] = (uint8_t)(m >> 24);
    i.mask[1] = (uint8_t)(m >> 16);
    i.mask[2] = (uint8_t)(m >> 8);
    i.mask[3] = (uint8_t)m;
    return i;
}

/* The access point rule (no person on the plant network can take the gateway). */
static void test_from_ap(void)
{
    const gw_claim_if_t ap = ifc(192, 168, 4, 1, 24);
    const gw_claim_if_t eth = ifc(10, 0, 0, 5, 24);
    const uint8_t ap_ip[4] = {192, 168, 4, 1}, eth_ip[4] = {10, 0, 0, 5};
    const uint8_t phone[4] = {192, 168, 4, 2}, plant[4] = {10, 0, 0, 77};
    /* a client of the access point */
    TEST_ASSERT_TRUE(gw_claim_from_ap(ap_ip, phone, &ap, &eth, 1));
    TEST_ASSERT_TRUE(gw_claim_from_ap(ap_ip, phone, &ap, NULL, 0));
    /* a client of the plant network, to the Ethernet address */
    TEST_ASSERT_FALSE(gw_claim_from_ap(eth_ip, plant, &ap, &eth, 1));
    /* weak host model: a plant host that sends to 192.168.4.1 through the Ethernet port */
    TEST_ASSERT_FALSE(gw_claim_from_ap(ap_ip, plant, &ap, &eth, 1));
    /* ... with a forged source address of the AP subnet, to the Ethernet address */
    TEST_ASSERT_FALSE(gw_claim_from_ap(eth_ip, phone, &ap, &eth, 1));
    /* the network, broadcast and own addresses of the AP subnet are not hosts */
    const uint8_t net[4] = {192, 168, 4, 0}, bcast[4] = {192, 168, 4, 255};
    TEST_ASSERT_FALSE(gw_claim_from_ap(ap_ip, net, &ap, NULL, 0));
    TEST_ASSERT_FALSE(gw_claim_from_ap(ap_ip, bcast, &ap, NULL, 0));
    TEST_ASSERT_FALSE(gw_claim_from_ap(ap_ip, ap_ip, &ap, NULL, 0));
    /* the plant network uses the same subnet: the route back is ambiguous, refused */
    const gw_claim_if_t eth_same = ifc(192, 168, 4, 20, 24);
    TEST_ASSERT_FALSE(gw_claim_from_ap(ap_ip, phone, &ap, &eth_same, 1));
    const gw_claim_if_t eth_wide = ifc(192, 168, 0, 20, 16);
    TEST_ASSERT_FALSE(gw_claim_from_ap(ap_ip, phone, &ap, &eth_wide, 1));
    /* an interface without an address (Wi-Fi station not connected) is ignored */
    const gw_claim_if_t none[2] = {ifc(0, 0, 0, 0, 0), eth};
    TEST_ASSERT_TRUE(gw_claim_from_ap(ap_ip, phone, &ap, none, 2));
    /* no access point (no address, or no netmask): nobody */
    const gw_claim_if_t no_ap = ifc(0, 0, 0, 0, 24), no_mask = ifc(192, 168, 4, 1, 0);
    const uint8_t zero[4] = {0, 0, 0, 0};
    TEST_ASSERT_FALSE(gw_claim_from_ap(zero, phone, &no_ap, NULL, 0));
    TEST_ASSERT_FALSE(gw_claim_from_ap(ap_ip, phone, &no_mask, NULL, 0));
    TEST_ASSERT_FALSE(gw_claim_from_ap(ap_ip, phone, NULL, NULL, 0));
    /* a /31 or /32 access point subnet has no host for a client: refused */
    const gw_claim_if_t p32 = ifc(192, 168, 4, 1, 32);
    TEST_ASSERT_FALSE(gw_claim_from_ap(ap_ip, phone, &p32, NULL, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_opens_and_closes_at_the_end_of_the_time);
    RUN_TEST(test_second_gesture_starts_the_time_again);
    RUN_TEST(test_never_opens_with_a_password_set);
    RUN_TEST(test_set_refused_while_closed);
    RUN_TEST(test_closes_after_a_set);
    RUN_TEST(test_only_from_the_access_point);
    RUN_TEST(test_password_rules);
    RUN_TEST(test_parse);
    RUN_TEST(test_from_ap);
    return UNITY_END();
}
