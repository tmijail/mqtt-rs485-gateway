/* NET-5, issue #187: the WPA2 key of the access point is the web password (gw_wpa.h). SHA-1,
 * HMAC-SHA1 and PBKDF2-HMAC-SHA1 with their published vectors, the PMK with the vectors of
 * IEEE 802.11i (annex H.4), the passphrase rule and the SSID. */
#include <stdio.h>
#include <string.h>

#include "gw_wpa.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", b[i]);
    }
}

/* FIPS 180-4 examples, and the million 'a' of FIPS 180-1. */
static void test_sha1_vectors(void)
{
    uint8_t d[GW_SHA1_LEN];
    char h[2 * GW_SHA1_LEN + 1];
    gw_sha1("abc", 3, d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("a9993e364706816aba3e25717850c26c9cd0d89d", h);
    gw_sha1("", 0, d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("da39a3ee5e6b4b0d3255bfef95601890afd80709", h);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";   /* 56 bytes */
    gw_sha1(two, strlen(two), d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("84983e441c3bd26ebaae4aa1f95129e5e54670f1", h);
    static char million[1000000];
    memset(million, 'a', sizeof(million));
    gw_sha1(million, sizeof(million), d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("34aa973cd4c4daa4f61eeb2bdbad27316534016f", h);
}

/* RFC 2202 test cases 1 and 6 (a key longer than the block). */
static void test_hmac_sha1_vectors(void)
{
    uint8_t key[80], d[GW_SHA1_LEN];
    char h[2 * GW_SHA1_LEN + 1];
    memset(key, 0x0b, 20);
    gw_hmac_sha1(key, 20, (const uint8_t *)"Hi There", 8, d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("b617318655057264e28bc0b6fb378c8ef146be00", h);
    memset(key, 0xaa, 80);
    const char *m = "Test Using Larger Than Block-Size Key - Hash Key First";
    gw_hmac_sha1(key, 80, (const uint8_t *)m, strlen(m), d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("aa4ae5e15272d00e95705637ce8a3b55ed402112", h);
}

/* RFC 6070 test cases 1 and 3. */
static void test_pbkdf2_sha1_vectors(void)
{
    uint8_t d[GW_SHA1_LEN];
    char h[2 * GW_SHA1_LEN + 1];
    gw_pbkdf2_sha1((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, d, sizeof(d));
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("0c60c80f961f0e71f3a9b524af6012062fe037a6", h);
    gw_pbkdf2_sha1((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, d, sizeof(d));
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("4b007901b765489abead49d926f721d065a429c1", h);
}

/* IEEE 802.11i-2004 annex H.4: the PSK of a passphrase and an SSID (two blocks of PBKDF2). */
static void test_pmk_ieee_80211i_vectors(void)
{
    char pmk[GW_WPA_PMK_HEX_SIZE];
    TEST_ASSERT_TRUE(gw_wpa_pmk_hex("password", "IEEE", pmk));
    TEST_ASSERT_EQUAL_STRING("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e",
                             pmk);
    TEST_ASSERT_TRUE(gw_wpa_pmk_hex("ThisIsAPassword", "ThisIsASSID", pmk));
    TEST_ASSERT_EQUAL_STRING("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af",
                             pmk);
    TEST_ASSERT_TRUE(gw_wpa_pmk_hex_valid(pmk));
    /* the binary form is the same key */
    uint8_t bin[GW_WPA_PMK_LEN];
    char h[GW_WPA_PMK_HEX_SIZE];
    TEST_ASSERT_TRUE(gw_wpa_pmk("ThisIsAPassword", "ThisIsASSID", bin));
    hex(bin, sizeof(bin), h);
    TEST_ASSERT_EQUAL_STRING(pmk, h);
}

/* The SSID of the gateway is the salt: the E2E computes the same value (hashlib). */
static void test_pmk_of_the_gateway_ssid(void)
{
    char ssid[GW_WPA_SSID_SIZE], pmk[GW_WPA_PMK_HEX_SIZE];
    const uint8_t mac[6] = {0x24, 0x0a, 0xc4, 0xab, 0xcd, 0xef};
    gw_wpa_ssid(mac, ssid);
    TEST_ASSERT_EQUAL_STRING("GW-Setup-ABCDEF", ssid);
    TEST_ASSERT_TRUE(gw_wpa_pmk_hex("e2e-claimed-pw", ssid, pmk));
    TEST_ASSERT_EQUAL_STRING("a32815b1ae4e4d276581e07825e93a1473fca90f26421d41e7229ab427cd6030",
                             pmk);
}

/* A passphrase or an SSID outside the rules gives no key. */
static void test_pmk_refuses_bad_input(void)
{
    char pmk[GW_WPA_PMK_HEX_SIZE] = "x";
    TEST_ASSERT_FALSE(gw_wpa_pmk_hex("short", "IEEE", pmk));
    TEST_ASSERT_EQUAL_STRING("", pmk);
    TEST_ASSERT_FALSE(gw_wpa_pmk_hex("password", "", pmk));
    TEST_ASSERT_FALSE(gw_wpa_pmk_hex("password", "123456789012345678901234567890123", pmk));
    TEST_ASSERT_TRUE(gw_wpa_pmk_hex("password", "12345678901234567890123456789012", pmk));
    TEST_ASSERT_FALSE(gw_wpa_pmk_hex("pass\xc3\xa9word", "IEEE", pmk));
}

/* WPA2 passphrase: 8 to 63 characters, each 0x20..0x7E (hostapd, wpa_supplicant). */
static void test_passphrase_rule(void)
{
    TEST_ASSERT_EQUAL(GW_WPA_PASS_OK, gw_wpa_pass_check("12345678"));
    TEST_ASSERT_EQUAL(GW_WPA_PASS_OK, gw_wpa_pass_check(" ~!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}"));
    TEST_ASSERT_EQUAL(GW_WPA_PASS_OK, gw_wpa_pass_check("        "));   /* 8 spaces */
    TEST_ASSERT_EQUAL(GW_WPA_PASS_SHORT, gw_wpa_pass_check(""));
    TEST_ASSERT_EQUAL(GW_WPA_PASS_SHORT, gw_wpa_pass_check("1234567"));
    char p[80];
    memset(p, 'a', sizeof(p));
    p[63] = '\0';
    TEST_ASSERT_EQUAL(GW_WPA_PASS_OK, gw_wpa_pass_check(p));
    p[63] = 'a';
    p[64] = '\0';
    TEST_ASSERT_EQUAL(GW_WPA_PASS_LONG, gw_wpa_pass_check(p));   /* 64: a PMK, not a passphrase */
    TEST_ASSERT_EQUAL(GW_WPA_PASS_BAD_CHAR, gw_wpa_pass_check("1234\t5678"));
    TEST_ASSERT_EQUAL(GW_WPA_PASS_BAD_CHAR, gw_wpa_pass_check("12345678\x7f"));
    TEST_ASSERT_EQUAL(GW_WPA_PASS_BAD_CHAR, gw_wpa_pass_check("contrase\xc3\xb1" "a"));
    TEST_ASSERT_EQUAL(GW_WPA_PASS_BAD_CHAR, gw_wpa_pass_check("12345678\x80"));
    /* 7 characters of UTF-8 in 8 bytes: short is checked on bytes, then the characters fail */
    TEST_ASSERT_EQUAL(GW_WPA_PASS_BAD_CHAR, gw_wpa_pass_check("\xc3\xa9" "123456"));
}

static void test_pmk_hex_valid(void)
{
    TEST_ASSERT_TRUE(gw_wpa_pmk_hex_valid(
        "F42C6FC52DF0EBEF9EBB4B90B38A5F902E83FE1B135A70E23AED762E9710A12E"));
    TEST_ASSERT_FALSE(gw_wpa_pmk_hex_valid(""));
    TEST_ASSERT_FALSE(gw_wpa_pmk_hex_valid(
        "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12"));    /* 63 */
    TEST_ASSERT_FALSE(gw_wpa_pmk_hex_valid(
        "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e0"));  /* 65 */
    TEST_ASSERT_FALSE(gw_wpa_pmk_hex_valid(
        "g42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
}

/* ---------- issue #269: the order of the writes of the hash and the PMK ---------- */

/* The two NVS entries. Each password of the test has a hash and a PMK with its name in them, so
 * that the test can tell whose PMK is stored. */
typedef struct {
    char hash[24];
    char pmk[GW_WPA_PMK_HEX_SIZE];
} pw_store_t;

#define PMK_OLD "0000000000000000000000000000000000000000000000000000000000000a1d"
#define PMK_NEW "00000000000000000000000000000000000000000000000000000000000002e3"

static void store_apply(pw_store_t *s, gw_wpa_store_op_t op, const char *hash, const char *pmk)
{
    switch (op) {
    case GW_WPA_STORE_ERASE_PMK: s->pmk[0] = '\0'; break;
    case GW_WPA_STORE_SET_HASH: snprintf(s->hash, sizeof(s->hash), "%s", hash); break;
    case GW_WPA_STORE_ERASE_HASH: s->hash[0] = '\0'; break;
    case GW_WPA_STORE_SET_PMK: snprintf(s->pmk, sizeof(s->pmk), "%s", pmk); break;
    default: TEST_FAIL_MESSAGE("not a store operation"); break;
    }
}

/* The PMK of the password whose hash is @p hash ("" for a password with no PMK, or none). */
static const char *pmk_of(const char *hash)
{
    return strcmp(hash, "hash-old") == 0 ? PMK_OLD : strcmp(hash, "hash-new") == 0 ? PMK_NEW : "";
}

/* A change from @p from to (@p hash, @p pmk), with a power cut after each number of writes: the
 * store never has a PMK that is not the PMK of its password, and the access point never uses
 * such a PMK. Returns the number of writes of the full change. */
static size_t cut_each_write(const pw_store_t *from, const char *hash, const char *pmk)
{
    gw_wpa_store_op_t ops[GW_WPA_STORE_OPS_MAX];
    size_t n = gw_wpa_store_plan(from->hash, from->pmk, hash, pmk, ops);
    TEST_ASSERT_TRUE(n <= GW_WPA_STORE_OPS_MAX);
    for (size_t cut = 0; cut <= n; cut++) {
        pw_store_t s = *from;
        for (size_t i = 0; i < cut; i++) {
            store_apply(&s, ops[i], hash, pmk);
        }
        char msg[200];
        snprintf(msg, sizeof(msg), "from ('%s','%.8s') to ('%s','%.8s'), cut after %u of %u "
                                   "writes: ('%s','%.8s')", from->hash, from->pmk, hash, pmk,
                 (unsigned)cut, (unsigned)n, s.hash, s.pmk);
        /* a stored PMK is the PMK of the stored password */
        if (s.pmk[0] != '\0' && s.hash[0] != '\0') {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(pmk_of(s.hash), s.pmk, msg);
        }
        /* a PMK without a password can only be one that was there before (an earlier fault) */
        if (s.pmk[0] != '\0' && s.hash[0] == '\0') {
            TEST_ASSERT_TRUE_MESSAGE(from->hash[0] == '\0' && cut == 0, msg);
        }
        /* the key of the access point: a PMK only when it is the PMK of the stored password */
        if (gw_wpa_key_is_pmk(s.hash, s.pmk)) {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(pmk_of(s.hash), s.pmk, msg);
        }
        /* the stored password is the old one or the new one */
        TEST_ASSERT_TRUE_MESSAGE(strcmp(s.hash, from->hash) == 0 || strcmp(s.hash, hash) == 0, msg);
        if (cut == n) {
            /* no cut: the new pair (a PMK without a hash is not written) */
            TEST_ASSERT_EQUAL_STRING_MESSAGE(hash, s.hash, msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(hash[0] != '\0' ? pmk : "", s.pmk, msg);
        }
    }
    return n;
}

static void test_store_order_with_a_cut_at_each_write(void)
{
    const pw_store_t none = {"", ""}, old = {"hash-old", PMK_OLD}, old_no_pmk = {"hash-old", ""};
    gw_wpa_store_op_t ops[GW_WPA_STORE_OPS_MAX];

    /* the first web password: the hash, then the PMK */
    TEST_ASSERT_EQUAL_size_t(2, gw_wpa_store_plan("", "", "hash-new", PMK_NEW, ops));
    TEST_ASSERT_EQUAL(GW_WPA_STORE_SET_HASH, ops[0]);
    TEST_ASSERT_EQUAL(GW_WPA_STORE_SET_PMK, ops[1]);
    TEST_ASSERT_EQUAL_size_t(2, cut_each_write(&none, "hash-new", PMK_NEW));

    /* a change of the web password: the old PMK goes first. Before #269 the hash was written
     * first, and a cut left the new hash with the PMK of the old password */
    TEST_ASSERT_EQUAL_size_t(3, gw_wpa_store_plan("hash-old", PMK_OLD, "hash-new", PMK_NEW, ops));
    TEST_ASSERT_EQUAL(GW_WPA_STORE_ERASE_PMK, ops[0]);
    TEST_ASSERT_EQUAL(GW_WPA_STORE_SET_HASH, ops[1]);
    TEST_ASSERT_EQUAL(GW_WPA_STORE_SET_PMK, ops[2]);
    TEST_ASSERT_EQUAL_size_t(3, cut_each_write(&old, "hash-new", PMK_NEW));

    /* the removal of the web password: the PMK goes first. Before #269 a cut left the PMK of
     * the old password with no hash, and the access point used it */
    TEST_ASSERT_EQUAL_size_t(2, gw_wpa_store_plan("hash-old", PMK_OLD, "", "", ops));
    TEST_ASSERT_EQUAL(GW_WPA_STORE_ERASE_PMK, ops[0]);
    TEST_ASSERT_EQUAL(GW_WPA_STORE_ERASE_HASH, ops[1]);
    TEST_ASSERT_EQUAL_size_t(2, cut_each_write(&old, "", ""));

    /* the password stays (a document with "********"): no write, so no time without a PMK */
    TEST_ASSERT_EQUAL_size_t(0, cut_each_write(&old, "hash-old", PMK_OLD));
    TEST_ASSERT_EQUAL_size_t(0, cut_each_write(&none, "", ""));
    TEST_ASSERT_EQUAL_size_t(0, cut_each_write(&old_no_pmk, "hash-old", ""));

    /* a new password whose PMK could not be calculated (no MAC address) */
    TEST_ASSERT_EQUAL_size_t(2, cut_each_write(&old, "hash-new", ""));
    TEST_ASSERT_EQUAL_size_t(1, cut_each_write(&none, "hash-new", ""));
    /* a password with no PMK (the fault of #248) gets a new password, or is removed */
    TEST_ASSERT_EQUAL_size_t(2, cut_each_write(&old_no_pmk, "hash-new", PMK_NEW));
    TEST_ASSERT_EQUAL_size_t(1, cut_each_write(&old_no_pmk, "", ""));
    /* a save that failed puts the old pair back */
    const pw_store_t new_pair = {"hash-new", PMK_NEW};
    TEST_ASSERT_EQUAL_size_t(3, cut_each_write(&new_pair, "hash-old", PMK_OLD));
    /* a PMK is never written without a hash */
    TEST_ASSERT_EQUAL_size_t(0, cut_each_write(&none, "", PMK_NEW));
    TEST_ASSERT_EQUAL_size_t(2, cut_each_write(&old, "", PMK_NEW));

    /* a PMK with no hash, from a cut of the firmware before #269: the next write removes it */
    const pw_store_t orphan = {"", PMK_OLD};
    TEST_ASSERT_EQUAL_size_t(1, gw_wpa_store_plan("", PMK_OLD, "", "", ops));
    TEST_ASSERT_EQUAL(GW_WPA_STORE_ERASE_PMK, ops[0]);
    TEST_ASSERT_EQUAL_size_t(3, cut_each_write(&orphan, "hash-new", PMK_NEW));
}

static void test_the_access_point_uses_a_pmk_only_with_a_web_password(void)
{
    TEST_ASSERT_TRUE(gw_wpa_key_is_pmk("pbkdf2-sha256$2048$00$00", PMK_OLD));
    TEST_ASSERT_FALSE(gw_wpa_key_is_pmk("", PMK_OLD));                         /* no web password */
    TEST_ASSERT_FALSE(gw_wpa_key_is_pmk("pbkdf2-sha256$2048$00$00", ""));      /* #248: no PMK */
    TEST_ASSERT_FALSE(gw_wpa_key_is_pmk("pbkdf2-sha256$2048$00$00", "0123"));  /* not a PMK */
    TEST_ASSERT_FALSE(gw_wpa_key_is_pmk("", ""));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_store_order_with_a_cut_at_each_write);
    RUN_TEST(test_the_access_point_uses_a_pmk_only_with_a_web_password);
    RUN_TEST(test_sha1_vectors);
    RUN_TEST(test_hmac_sha1_vectors);
    RUN_TEST(test_pbkdf2_sha1_vectors);
    RUN_TEST(test_pmk_ieee_80211i_vectors);
    RUN_TEST(test_pmk_of_the_gateway_ssid);
    RUN_TEST(test_pmk_refuses_bad_input);
    RUN_TEST(test_passphrase_rule);
    RUN_TEST(test_pmk_hex_valid);
    return UNITY_END();
}
