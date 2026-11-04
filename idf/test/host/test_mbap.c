/* QA-1: Modbus TCP gateway framing (MB-15): MBAP <-> RTU, the gateway's own exceptions and the
 * client allow-list. Transactions run against the simulated RTU slave. */
#include <string.h>

#include "gw_mbap.h"
#include "support.h"
#include "unity.h"

static sim_slave_t s;
static mb_master_t mb;
static gw_mbap_txn_t txn;
static uint8_t out[GW_MBAP_ADU_MAX];

void setUp(void)
{
    sim_reset(&s, 1);
    mb_transport_t t = sim_transport(&s);
    mb_master_cfg_t mc = {.baud = 9600, .response_timeout_ms = 200, .retries = 2,
                          .inter_frame_delay_ms = 5};
    mb_master_init(&mb, &t, &mc);
}
void tearDown(void) {}

/* MBAP frame: tid 0x1234, protocol 0, length, unit, then the PDU. */
static size_t frame(uint8_t *f, uint8_t unit, const uint8_t *pdu, size_t plen)
{
    f[0] = 0x12;
    f[1] = 0x34;
    f[2] = f[3] = 0;
    f[4] = (uint8_t)((plen + 1) >> 8);
    f[5] = (uint8_t)(plen + 1);
    f[6] = unit;
    memcpy(&f[7], pdu, plen);
    return 7 + plen;
}

static void test_frame_length(void)
{
    uint8_t f[GW_MBAP_ADU_MAX];
    size_t n = frame(f, 1, (const uint8_t[]){0x03, 0x00, 0x0A, 0x00, 0x02}, 5);
    TEST_ASSERT_EQUAL(12, n);
    TEST_ASSERT_EQUAL(0, gw_mbap_frame_len(f, 5));          /* header incomplete */
    TEST_ASSERT_EQUAL(12, gw_mbap_frame_len(f, 6));
    TEST_ASSERT_EQUAL(12, gw_mbap_frame_len(f, 12));
    f[3] = 1;                                                /* protocol id must be 0 */
    TEST_ASSERT_EQUAL(SIZE_MAX, gw_mbap_frame_len(f, 12));
    f[3] = 0;
    f[4] = 1;                                                /* length 262 > 254 */
    TEST_ASSERT_EQUAL(SIZE_MAX, gw_mbap_frame_len(f, 12));
    f[4] = 0;
    f[5] = 1;                                                /* unit id only, no function */
    TEST_ASSERT_EQUAL(SIZE_MAX, gw_mbap_frame_len(f, 12));
}

static void test_read_forwarded(void)
{
    s.holding[10] = 0x1234;
    s.holding[11] = 0xBEEF;
    uint8_t f[GW_MBAP_ADU_MAX];
    size_t n = frame(f, 1, (const uint8_t[]){0x03, 0x00, 0x0A, 0x00, 0x02}, 5);
    TEST_ASSERT_EQUAL(0, gw_mbap_parse(f, n, &txn));
    TEST_ASSERT_EQUAL(1, txn.rq.unit);
    TEST_ASSERT_EQUAL(MB_FC_READ_HOLDING_REGISTERS, txn.rq.fc);
    gw_mbap_execute(&mb, &txn);
    TEST_ASSERT_EQUAL(MB_OK, txn.status);
    size_t len = gw_mbap_response(&txn, out, sizeof(out));
    const uint8_t want[] = {0x12, 0x34, 0, 0, 0, 7, 1, 0x03, 4, 0x12, 0x34, 0xBE, 0xEF};
    TEST_ASSERT_EQUAL(sizeof(want), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, sizeof(want));
    /* coils too: 3 bits */
    s.coils[0] = s.coils[2] = true;
    n = frame(f, 1, (const uint8_t[]){0x01, 0x00, 0x00, 0x00, 0x03}, 5);
    TEST_ASSERT_EQUAL(0, gw_mbap_parse(f, n, &txn));
    gw_mbap_execute(&mb, &txn);
    len = gw_mbap_response(&txn, out, sizeof(out));
    TEST_ASSERT_EQUAL(10, len);
    TEST_ASSERT_EQUAL_HEX8(0x05, out[9]);
}

static void test_writes_forwarded(void)
{
    uint8_t f[GW_MBAP_ADU_MAX];
    size_t n = frame(f, 1, (const uint8_t[]){0x06, 0x00, 0x14, 0x10, 0x92}, 5);
    TEST_ASSERT_EQUAL(0, gw_mbap_parse(f, n, &txn));
    gw_mbap_execute(&mb, &txn);
    TEST_ASSERT_EQUAL_HEX16(0x1092, s.holding[20]);
    size_t len = gw_mbap_response(&txn, out, sizeof(out));
    TEST_ASSERT_EQUAL(n, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f, out, n);                 /* FC06 answer = echo */
    n = frame(f, 1, (const uint8_t[]){0x10, 0x00, 0x1E, 0x00, 0x02, 0x04, 0xAA, 0xBB, 0xCC, 0xDD},
              10);
    TEST_ASSERT_EQUAL(0, gw_mbap_parse(f, n, &txn));
    gw_mbap_execute(&mb, &txn);
    TEST_ASSERT_EQUAL_HEX16(0xAABB, s.holding[30]);
    TEST_ASSERT_EQUAL_HEX16(0xCCDD, s.holding[31]);
    len = gw_mbap_response(&txn, out, sizeof(out));
    const uint8_t want16[] = {0x12, 0x34, 0, 0, 0, 6, 1, 0x10, 0x00, 0x1E, 0x00, 0x02};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want16, out, sizeof(want16));
    TEST_ASSERT_EQUAL(sizeof(want16), len);
    n = frame(f, 1, (const uint8_t[]){0x05, 0x00, 0x03, 0xFF, 0x00}, 5);
    TEST_ASSERT_EQUAL(0, gw_mbap_parse(f, n, &txn));
    gw_mbap_execute(&mb, &txn);
    TEST_ASSERT_TRUE(s.coils[3]);
    TEST_ASSERT_EQUAL(n, gw_mbap_response(&txn, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(f, out, n);
    n = frame(f, 1, (const uint8_t[]){0x0F, 0x00, 0x08, 0x00, 0x03, 0x01, 0x05}, 7);
    TEST_ASSERT_EQUAL(0, gw_mbap_parse(f, n, &txn));
    gw_mbap_execute(&mb, &txn);
    TEST_ASSERT_TRUE(s.coils[8]);
    TEST_ASSERT_FALSE(s.coils[9]);
    TEST_ASSERT_TRUE(s.coils[10]);
}

static void test_gateway_exceptions(void)
{
    uint8_t f[GW_MBAP_ADU_MAX];
    /* functions the gateway does not forward */
    size_t n = frame(f, 1, (const uint8_t[]){0x2B, 0x0E, 0x01, 0x00}, 4);
    TEST_ASSERT_EQUAL(GW_MBAP_EX_ILLEGAL_FUNCTION, gw_mbap_parse(f, n, &txn));
    size_t len = gw_mbap_exception(f, GW_MBAP_EX_ILLEGAL_FUNCTION, out, sizeof(out));
    const uint8_t want[] = {0x12, 0x34, 0, 0, 0, 3, 1, 0xAB, 0x01};
    TEST_ASSERT_EQUAL(sizeof(want), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, sizeof(want));
    /* no such RTU unit: broadcast and 248..255 */
    const uint8_t rd[] = {0x03, 0x00, 0x00, 0x00, 0x01};
    n = frame(f, 0, rd, 5);
    TEST_ASSERT_EQUAL(GW_MBAP_EX_PATH_UNAVAILABLE, gw_mbap_parse(f, n, &txn));
    n = frame(f, 255, rd, 5);
    TEST_ASSERT_EQUAL(GW_MBAP_EX_PATH_UNAVAILABLE, gw_mbap_parse(f, n, &txn));
    /* bad quantities and values */
    n = frame(f, 1, (const uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x00}, 5);
    TEST_ASSERT_EQUAL(GW_MBAP_EX_ILLEGAL_VALUE, gw_mbap_parse(f, n, &txn));
    n = frame(f, 1, (const uint8_t[]){0x04, 0x00, 0x00, 0x00, 126}, 5);
    TEST_ASSERT_EQUAL(GW_MBAP_EX_ILLEGAL_VALUE, gw_mbap_parse(f, n, &txn));
    n = frame(f, 1, (const uint8_t[]){0x03, 0xFF, 0xFF, 0x00, 0x02}, 5);   /* past 65535 */
    TEST_ASSERT_EQUAL(GW_MBAP_EX_ILLEGAL_VALUE, gw_mbap_parse(f, n, &txn));
    n = frame(f, 1, (const uint8_t[]){0x05, 0x00, 0x00, 0x12, 0x34}, 5);
    TEST_ASSERT_EQUAL(GW_MBAP_EX_ILLEGAL_VALUE, gw_mbap_parse(f, n, &txn));
    n = frame(f, 1, (const uint8_t[]){0x10, 0x00, 0x00, 0x00, 0x02, 0x03, 1, 2, 3}, 9);
    TEST_ASSERT_EQUAL(GW_MBAP_EX_ILLEGAL_VALUE, gw_mbap_parse(f, n, &txn));
    n = frame(f, 1, (const uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x01, 0x00}, 6);  /* trailing */
    TEST_ASSERT_EQUAL(GW_MBAP_EX_ILLEGAL_VALUE, gw_mbap_parse(f, n, &txn));
    n = frame(f, 1, (const uint8_t[]){0x03}, 1);
    TEST_ASSERT_EQUAL(GW_MBAP_EX_ILLEGAL_VALUE, gw_mbap_parse(f, n, &txn));
}

static void test_slave_outcomes(void)
{
    uint8_t f[GW_MBAP_ADU_MAX];
    size_t n = frame(f, 1, (const uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x01}, 5);
    /* the slave answers an exception: passed through */
    s.exception = 2;
    TEST_ASSERT_EQUAL(0, gw_mbap_parse(f, n, &txn));
    gw_mbap_execute(&mb, &txn);
    size_t len = gw_mbap_response(&txn, out, sizeof(out));
    TEST_ASSERT_EQUAL(9, len);
    TEST_ASSERT_EQUAL_HEX8(0x83, out[7]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[8]);
    /* no answer after the retries: 0B, gateway target device failed to respond */
    s.exception = 0;
    s.offline = true;
    unsigned before = s.requests;
    gw_mbap_execute(&mb, &txn);
    TEST_ASSERT_EQUAL(before + 3, s.requests);
    len = gw_mbap_response(&txn, out, sizeof(out));
    TEST_ASSERT_EQUAL_HEX8(0x83, out[7]);
    TEST_ASSERT_EQUAL_HEX8(GW_MBAP_EX_TARGET_NO_ANSWER, out[8]);
    TEST_ASSERT_EQUAL(9, len);
}

static void test_client_allow_list(void)
{
    static gw_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    const uint8_t a[4] = {10, 0, 2, 2}, b[4] = {192, 168, 7, 99}, c[4] = {192, 168, 8, 1};
    TEST_ASSERT_TRUE(gw_mbap_client_allowed(&cfg, a));       /* empty list: anyone */
    cfg.modbus_tcp.client_count = 2;
    memcpy(cfg.modbus_tcp.clients[0].ip, (const uint8_t[]){10, 0, 2, 2}, 4);
    cfg.modbus_tcp.clients[0].prefix = 32;
    memcpy(cfg.modbus_tcp.clients[1].ip, (const uint8_t[]){192, 168, 7, 0}, 4);
    cfg.modbus_tcp.clients[1].prefix = 24;
    TEST_ASSERT_TRUE(gw_mbap_client_allowed(&cfg, a));
    TEST_ASSERT_TRUE(gw_mbap_client_allowed(&cfg, b));
    TEST_ASSERT_FALSE(gw_mbap_client_allowed(&cfg, c));
    TEST_ASSERT_FALSE(gw_mbap_client_allowed(&cfg, (const uint8_t[]){10, 0, 2, 3}));
    cfg.modbus_tcp.clients[1].prefix = 0;                     /* 0.0.0.0/0 */
    TEST_ASSERT_TRUE(gw_mbap_client_allowed(&cfg, c));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_length);
    RUN_TEST(test_read_forwarded);
    RUN_TEST(test_writes_forwarded);
    RUN_TEST(test_gateway_exceptions);
    RUN_TEST(test_slave_outcomes);
    RUN_TEST(test_client_allow_list);
    return UNITY_END();
}
