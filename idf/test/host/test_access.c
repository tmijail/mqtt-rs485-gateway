/* Issue #280: the answer of the shell command 'access' (gw_access.h). The install tool of #219
 * reads this text as data: its form is fixed. */
#include <string.h>

#include "gw_access.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static char s_out[GW_ACCESS_TEXT_MAX];

static const char *fmt(const gw_access_t *a)
{
    size_t n = gw_access_format(a, s_out, sizeof(s_out));
    TEST_ASSERT_TRUE(n < sizeof(s_out));
    TEST_ASSERT_EQUAL(strlen(s_out), n);
    return s_out;
}

static gw_access_t new_board(void)
{
    gw_access_t a = {
        .mode = "commissioning", .firmware = "0.2.0", .hostname = "gw01", .mdns = true,
        .web_running = true, .https = true, .port = 443, .password_set = false,
        .commissioning_password = "k7Qm2xPa9TbE", .fingerprint = "4F:9A:21",
        .ifs = {{.name = "eth", .state = GW_ACCESS_IF_UP, .ip = {192, 168, 1, 57}},
                {.name = "wifi", .state = GW_ACCESS_IF_OFF},
                {.name = "ap", .state = GW_ACCESS_IF_OFF, .no_mdns = true}},
        .n_ifs = 3,
    };
    return a;
}

/* A new board on Ethernet: the full answer, line for line. */
static void test_a_new_board_on_ethernet(void)
{
    gw_access_t a = new_board();
    TEST_ASSERT_EQUAL_STRING(
        "access-data v1\n"
        "mode commissioning\n"
        "firmware 0.2.0\n"
        "hostname gw01\n"
        "web on https 443\n"
        "login commissioning\n"
        "user admin\n"
        "commissioning-password k7Qm2xPa9TbE\n"
        "fingerprint 4F:9A:21\n"
        "if eth up 192.168.1.57\n"
        "if wifi off\n"
        "if ap off\n"
        "url https://192.168.1.57/\n"
        "url https://gw01.local/\n"
        "access-data end\n",
        fmt(&a));
}

/* A web password is set: the answer never has the commissioning password. */
static void test_a_web_password_hides_the_commissioning_password(void)
{
    gw_access_t a = new_board();
    a.mode = "normal";
    a.password_set = true;
    a.username = "operator";
    const char *t = fmt(&a);
    TEST_ASSERT_NOT_NULL(strstr(t, "login web-password\nuser operator\n"));
    TEST_ASSERT_NULL(strstr(t, "commissioning-password"));
    TEST_ASSERT_NULL(strstr(t, "k7Qm2xPa9TbE"));
    /* recovery and commissioning with a stored password: any user name */
    a.username = "";
    t = fmt(&a);
    TEST_ASSERT_NOT_NULL(strstr(t, "login web-password-any-user\n"));
    TEST_ASSERT_NULL(strstr(t, "\nuser "));
    TEST_ASSERT_NULL(strstr(t, "k7Qm2xPa9TbE"));
}

/* No address, and the web UI off: no URL, and the cause. */
static void test_no_url_gives_the_cause(void)
{
    gw_access_t a = new_board();
    a.ifs[0].state = GW_ACCESS_IF_NO_LINK;
    const char *t = fmt(&a);
    TEST_ASSERT_NOT_NULL(strstr(t, "if eth no-link\n"));
    TEST_ASSERT_NOT_NULL(strstr(t, "url-none no-address\n"));
    TEST_ASSERT_NULL(strstr(t, "\nurl "));
    a.ifs[0].state = GW_ACCESS_IF_NO_ADDRESS;
    TEST_ASSERT_NOT_NULL(strstr(fmt(&a), "if eth no-address\nif wifi off\n"));
    a = new_board();
    a.web_running = false;
    t = fmt(&a);
    TEST_ASSERT_NOT_NULL(strstr(t, "web off\n"));
    TEST_ASSERT_NOT_NULL(strstr(t, "url-none web-off\n"));
    TEST_ASSERT_NULL(strstr(t, "\nurl "));
    TEST_ASSERT_NULL(strstr(t, "fingerprint"));   /* no HTTPS server, no certificate in use */
}

/* Other ports, plain HTTP, two interfaces and the access point. */
static void test_ports_http_and_the_access_point(void)
{
    gw_access_t a = new_board();
    a.port = 8443;
    TEST_ASSERT_NOT_NULL(strstr(fmt(&a), "web on https 8443\n"));
    TEST_ASSERT_NOT_NULL(strstr(fmt(&a), "url https://192.168.1.57:8443/\nurl https://gw01.local:8443/\n"));
    a.https = false;
    a.port = 80;
    const char *t = fmt(&a);
    TEST_ASSERT_NOT_NULL(strstr(t, "web on http 80\n"));
    TEST_ASSERT_NOT_NULL(strstr(t, "url http://192.168.1.57/\n"));
    TEST_ASSERT_NULL(strstr(t, "fingerprint"));
    a = new_board();
    a.ifs[1] = (gw_access_if_t){.name = "wifi", .state = GW_ACCESS_IF_UP, .ip = {10, 0, 0, 9}};
    a.ifs[2] = (gw_access_if_t){.name = "ap", .state = GW_ACCESS_IF_UP, .ip = {192, 168, 4, 1},
                                .no_mdns = true};
    TEST_ASSERT_NOT_NULL(strstr(fmt(&a), "url https://192.168.1.57/\nurl https://10.0.0.9/\n"
                                         "url https://192.168.4.1/\nurl https://gw01.local/\n"));
    /* only the access point: its address, and no mDNS name (mDNS does not answer there) */
    a.ifs[0].state = GW_ACCESS_IF_NO_LINK;
    a.ifs[1].state = GW_ACCESS_IF_OFF;
    t = fmt(&a);
    TEST_ASSERT_NOT_NULL(strstr(t, "url https://192.168.4.1/\naccess-data end\n"));
    TEST_ASSERT_NULL(strstr(t, ".local"));
    /* mDNS off, or no host name: no mDNS URL */
    a = new_board();
    a.mdns = false;
    TEST_ASSERT_NULL(strstr(fmt(&a), ".local"));
    a = new_board();
    a.hostname = "";
    t = fmt(&a);
    TEST_ASSERT_NULL(strstr(t, ".local"));
    TEST_ASSERT_NULL(strstr(t, "hostname"));
}

/* A value of the configuration cannot make a line of its own. */
static void test_a_value_cannot_make_a_new_line(void)
{
    gw_access_t a = new_board();
    a.password_set = true;
    a.username = "bad\nmode normal\r";
    const char *t = fmt(&a);
    TEST_ASSERT_NOT_NULL(strstr(t, "user bad?mode normal?\n"));
    TEST_ASSERT_NULL(strstr(t, "\nmode normal"));
}

/* A buffer that is too small: the length of the full text, and a text that ends with a NUL. */
static void test_a_small_buffer(void)
{
    gw_access_t a = new_board();
    size_t full = gw_access_format(&a, s_out, sizeof(s_out));
    char small[40];
    memset(small, 'x', sizeof(small));
    TEST_ASSERT_EQUAL(full, gw_access_format(&a, small, sizeof(small)));
    TEST_ASSERT_EQUAL(sizeof(small) - 1, strlen(small));
    TEST_ASSERT_EQUAL(full, gw_access_format(&a, NULL, 0));
    /* the longest answer fits GW_ACCESS_TEXT_MAX */
    a = new_board();
    a.port = 65535;
    a.hostname = "a23456789012345678901234567890123456789012345678901234567890123";
    a.firmware = "0.2.0+g12345678.dirty";
    a.fingerprint = "4F:9A:21:C3:7B:E0:55:18:AD:02:6E:91:F3:3C:88:D4:10:7A:BE:59:C6:2D:E7:04:9B:31:F8:"
                    "6A:5D:C0:13:E2";
    a.ifs[0].ip[0] = a.ifs[0].ip[1] = a.ifs[0].ip[2] = a.ifs[0].ip[3] = 255;
    a.ifs[1] = a.ifs[0];
    a.ifs[1].name = "wifi";
    a.ifs[2] = a.ifs[0];
    a.ifs[2].name = "ap";
    TEST_ASSERT_TRUE(gw_access_format(&a, s_out, sizeof(s_out)) < GW_ACCESS_TEXT_MAX);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_new_board_on_ethernet);
    RUN_TEST(test_a_web_password_hides_the_commissioning_password);
    RUN_TEST(test_no_url_gives_the_cause);
    RUN_TEST(test_ports_http_and_the_access_point);
    RUN_TEST(test_a_value_cannot_make_a_new_line);
    RUN_TEST(test_a_small_buffer);
    return UNITY_END();
}
