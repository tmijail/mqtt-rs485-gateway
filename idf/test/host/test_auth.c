/* QA-1: web credentials (CFG-3, CFG-4, CFG-7): SHA-256, HMAC, PBKDF2, Basic auth. */
#include <stdio.h>
#include <string.h>

#include "gw_auth.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void hex(const uint8_t *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", b[i]);
    }
}

static void assert_sha(const char *want, const void *data, size_t len)
{
    uint8_t d[GW_SHA256_LEN];
    char h[2 * GW_SHA256_LEN + 1];
    gw_sha256(data, len, d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING(want, h);
}

/* FIPS 180-4 examples, and the padding boundaries (55, 56, 64 bytes). */
static void test_sha256_vectors(void)
{
    assert_sha("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "", 0);
    assert_sha("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc", 3);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    assert_sha("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", two,
               strlen(two));
    static char a[1000000];
    memset(a, 'a', sizeof(a));
    assert_sha("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", a, 55);
    assert_sha("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", a, 56);
    assert_sha("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", a, 64);
    /* one million 'a' fed in uneven pieces */
    gw_sha256_t s;
    gw_sha256_init(&s);
    for (size_t off = 0, step = 1; off < sizeof(a); off += step, step = step % 97 + 1) {
        gw_sha256_update(&s, a + off, off + step <= sizeof(a) ? step : sizeof(a) - off);
    }
    uint8_t d[GW_SHA256_LEN];
    char h[65];
    gw_sha256_final(&s, d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", h);
}

/* RFC 4231 test cases 1, 2 and 6 (key longer than a block). */
static void test_hmac_vectors(void)
{
    uint8_t key[131], d[GW_SHA256_LEN];
    char h[65];
    memset(key, 0x0b, 20);
    gw_hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", h);
    const char *msg = "what do ya want for nothing?";
    gw_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)msg, strlen(msg), d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", h);
    memset(key, 0xaa, sizeof(key));
    msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    gw_hmac_sha256(key, sizeof(key), (const uint8_t *)msg, strlen(msg), d);
    hex(d, sizeof(d), h);
    TEST_ASSERT_EQUAL_STRING("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", h);
}

/* RFC 7914 section 11 and the widely published PBKDF2-HMAC-SHA256 vectors. */
static void test_pbkdf2_vectors(void)
{
    uint8_t d[64];
    char h[129];
    gw_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, d, 32);
    hex(d, 32, h);
    TEST_ASSERT_EQUAL_STRING("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b", h);
    gw_pbkdf2_sha256((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 4096, d, 32);
    hex(d, 32, h);
    TEST_ASSERT_EQUAL_STRING("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a", h);
    gw_pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1, d, 64);
    hex(d, 64, h);
    TEST_ASSERT_EQUAL_STRING("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                             "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
                             h);
    const char *pw = "passwordPASSWORDpassword", *salt = "saltSALTsaltSALTsaltSALTsaltSALTsalt";
    gw_pbkdf2_sha256((const uint8_t *)pw, strlen(pw), (const uint8_t *)salt, strlen(salt), 4096,
                     d, 40);
    hex(d, 40, h);
    TEST_ASSERT_EQUAL_STRING(
        "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9", h);
}

static void test_password_hash_roundtrip(void)
{
    uint8_t salt[GW_AUTH_SALT_LEN];
    for (unsigned i = 0; i < sizeof(salt); i++) {
        salt[i] = (uint8_t)i;
    }
    char enc[128];
    TEST_ASSERT_TRUE(gw_auth_hash_password("e2e-pass-1", salt, 2048, enc, sizeof(enc)));
    /* same derivation as Python's hashlib.pbkdf2_hmac('sha256', ..., 2048) */
    TEST_ASSERT_EQUAL_STRING("pbkdf2-sha256$2048$000102030405060708090a0b0c0d0e0f$"
                             "00eee7f43dc745cb2048a91a5ee299d6e1cbc4f872b44b602f1b28715674825e",
                             enc);
    TEST_ASSERT_TRUE(gw_auth_check_password("e2e-pass-1", enc));
    TEST_ASSERT_FALSE(gw_auth_check_password("e2e-pass-2", enc));
    TEST_ASSERT_FALSE(gw_auth_check_password("", enc));
    TEST_ASSERT_FALSE(gw_auth_check_password("e2e-pass-1", ""));
    TEST_ASSERT_FALSE(gw_auth_check_password("e2e-pass-1", "plain-text"));
    char bad[128];
    strcpy(bad, enc);
    bad[strlen(bad) - 1] ^= 1;   /* altered hash */
    TEST_ASSERT_FALSE(gw_auth_check_password("e2e-pass-1", bad));
    TEST_ASSERT_FALSE(gw_auth_check_password("e2e-pass-1", "pbkdf2-sha256$0$00$00"));
    TEST_ASSERT_FALSE(gw_auth_hash_password("x", salt, 2048, enc, 40));   /* does not fit */
}

static void test_basic_auth_header(void)
{
    char u[33], p[65];
    TEST_ASSERT_TRUE(gw_auth_parse_basic("Basic YWRtaW46c2U6Y3JldA==", u, sizeof(u), p, sizeof(p)));
    TEST_ASSERT_EQUAL_STRING("admin", u);
    TEST_ASSERT_EQUAL_STRING("se:cret", p);   /* only the first ':' separates */
    TEST_ASSERT_TRUE(gw_auth_parse_basic("Basic YTo=", u, sizeof(u), p, sizeof(p)));   /* "a:" */
    TEST_ASSERT_EQUAL_STRING("", p);
    TEST_ASSERT_FALSE(gw_auth_parse_basic("Bearer YWRtaW46eA==", u, sizeof(u), p, sizeof(p)));
    TEST_ASSERT_FALSE(gw_auth_parse_basic("Basic YWRtaW4=", u, sizeof(u), p, sizeof(p)));  /* no ':' */
    TEST_ASSERT_FALSE(gw_auth_parse_basic("Basic YWRta!46eA==", u, sizeof(u), p, sizeof(p)));
    TEST_ASSERT_FALSE(gw_auth_parse_basic("Basic YWRtaW46eA", u, sizeof(u), p, sizeof(p)));
    TEST_ASSERT_FALSE(gw_auth_parse_basic(NULL, u, sizeof(u), p, sizeof(p)));
    TEST_ASSERT_FALSE(gw_auth_parse_basic("Basic YWRtaW46c2U6Y3JldA==", u, 5, p, sizeof(p)));
    uint8_t out[4];
    TEST_ASSERT_EQUAL(-1, gw_base64_decode("YWJjZA==", 8, out, 3));   /* overflow */
    TEST_ASSERT_EQUAL(4, gw_base64_decode("YWJjZA==", 8, out, 4));
    TEST_ASSERT_EQUAL_MEMORY("abcd", out, 4);
    TEST_ASSERT_TRUE(gw_auth_equal("abc", "abc", 3));
    TEST_ASSERT_FALSE(gw_auth_equal("abc", "abd", 3));
}

/* Issue #100: state-changing requests need the custom header and, from a browser, the same
 * origin as the Host header. */
static void test_csrf_check(void)
{
    const char *xrw = "XMLHttpRequest";
    /* not a browser: the header is enough */
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, "10.0.0.5", NULL, NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, NULL, "", "", xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_NO_HEADER, gw_auth_csrf_check(true, "10.0.0.5", NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(GW_CSRF_NO_HEADER, gw_auth_csrf_check(true, "10.0.0.5", NULL, NULL, ""));
    /* the header is required even from the same origin */
    TEST_ASSERT_EQUAL(GW_CSRF_NO_HEADER,
                      gw_auth_csrf_check(true, "10.0.0.5", "https://10.0.0.5", NULL, NULL));
    /* same origin: scheme, host and port; default ports, case */
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, "10.0.0.5", "https://10.0.0.5", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK,
                      gw_auth_csrf_check(true, "10.0.0.5:443", "https://10.0.0.5", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK,
                      gw_auth_csrf_check(true, "GW01.local", "HTTPS://gw01.LOCAL:443", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK,
                      gw_auth_csrf_check(true, "127.0.0.1:8443", "https://127.0.0.1:8443", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(false, "gw:80", "http://gw", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK,
                      gw_auth_csrf_check(true, "[fe80::1]:8443", "https://[fe80::1]:8443", NULL, xrw));
    /* another site, port or scheme */
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, "10.0.0.5", "https://evil.example", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, "10.0.0.5", "https://10.0.0.5:8443", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, "10.0.0.5", "http://10.0.0.5", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, "10.0.0.5", "https://10.0.0.5.evil.example", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN, gw_auth_csrf_check(true, "10.0.0.5", "null", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, "10.0.0.5", "https://user@10.0.0.5", NULL, xrw));
    /* no Host to compare with */
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, NULL, "https://10.0.0.5", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, "10.0.0.5/x", "https://10.0.0.5", NULL, xrw));
    /* Referer only when there is no Origin; its path does not matter */
    TEST_ASSERT_EQUAL(GW_CSRF_OK,
                      gw_auth_csrf_check(true, "10.0.0.5", NULL, "https://10.0.0.5/?tab=cfg", xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, "10.0.0.5", NULL, "https://evil.example/10.0.0.5", xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, "10.0.0.5", "https://evil.example",
                                         "https://10.0.0.5/", xrw));
    /* issue #106: canonical forms (trailing dot, leading zeros, empty or explicit default
     * port, upper-case IPv6) compare equal; malformed authorities are refused */
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, "gw01.local.", "https://GW01.local", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, "gw01.local", "https://gw01.local.:443", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, "10.0.0.5:0443", "https://10.0.0.5", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, "10.0.0.5:", "https://10.0.0.5:443", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, "10.0.0.5:08443", "https://10.0.0.5:8443", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, "[FE80::A]:443", "https://[fe80::a]", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(false, "gw", "http://gw:80/", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_OK, gw_auth_csrf_check(true, "gw.plant.example", "https://gw.plant.example", NULL, xrw));
    /* the same device under two names is still two origins */
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN, gw_auth_csrf_check(true, "10.0.0.5", "https://gw01.local", NULL, xrw));
    /* the scheme's default only: 80 is not the default of HTTPS */
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN, gw_auth_csrf_check(true, "gw:80", "https://gw", NULL, xrw));
    static const char *const bad[] = {"10.0.0.5:99999", "10.0.0.5:0", "10.0.0.5:123456", "gw:1:2",
                                      "g w", " gw", "gw\t", "[fe80::1", "[]", "[fe80::1]x", "[fe80::g]",
                                      "u@gw", ":443", ".", "gw:44a"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char origin[64];
        snprintf(origin, sizeof(origin), "https://%s", bad[i]);
        TEST_ASSERT_EQUAL_MESSAGE(GW_CSRF_CROSS_ORIGIN, gw_auth_csrf_check(true, bad[i], origin, NULL, xrw), bad[i]);
        TEST_ASSERT_EQUAL_MESSAGE(GW_CSRF_CROSS_ORIGIN, gw_auth_csrf_check(true, "gw", origin, NULL, xrw), bad[i]);
    }
    /* a Host header is an authority only (in a URL, '/', '?' or '#' end it) */
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN, gw_auth_csrf_check(true, "gw?x", "https://gw", NULL, xrw));
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN, gw_auth_csrf_check(true, "gw#x", "https://gw", NULL, xrw));
    /* an authority longer than the buffer is refused, not cut */
    char longhost[200];
    memset(longhost, 'a', sizeof(longhost) - 1);
    longhost[sizeof(longhost) - 1] = '\0';
    char longorigin[220];
    snprintf(longorigin, sizeof(longorigin), "https://%s", longhost);
    TEST_ASSERT_EQUAL(GW_CSRF_CROSS_ORIGIN,
                      gw_auth_csrf_check(true, longhost, longorigin, NULL, xrw));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sha256_vectors);
    RUN_TEST(test_hmac_vectors);
    RUN_TEST(test_pbkdf2_vectors);
    RUN_TEST(test_password_hash_roundtrip);
    RUN_TEST(test_basic_auth_header);
    RUN_TEST(test_csrf_check);
    return UNITY_END();
}
