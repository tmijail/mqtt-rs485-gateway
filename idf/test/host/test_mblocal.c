/* #216: the local unit of the Modbus TCP server: the DIs (FC02), the DOs (FC01, FC05, FC15) and
 * the pulse counters (FC04, uint64 big) of the gateway, from a snapshot of its I/O. */
#include <string.h>

#include "gw_mblocal.h"
#include "support.h"
#include "unity.h"

static gw_config_t cfg;
static gw_mblocal_io_t io;
static gw_mbap_txn_t txn;
static gw_mblocal_write_t wr;
static uint8_t out[GW_MBAP_ADU_MAX];

void setUp(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.modbus_tcp.local_unit_id = 100;
    memset(&io, 0, sizeof(io));
    io.di = 0x05;                     /* DI/1 and DI/3 */
    io.dout = 0x82;                   /* DO/2 and DO/8 */
    io.do_known = true;
    io.counters[0] = 0x0102030405060708ull;
    io.counters[7] = 42;
}
void tearDown(void) {}

/* Parse the MBAP frame of @p pdu for unit 100 and answer it; the length of the response in out. */
static size_t run(const uint8_t *pdu, size_t plen)
{
    uint8_t f[GW_MBAP_ADU_MAX] = {0x12, 0x34, 0, 0, (uint8_t)((plen + 1) >> 8), (uint8_t)(plen + 1),
                                  100};
    memcpy(&f[7], pdu, plen);
    TEST_ASSERT_EQUAL_UINT8(0, gw_mbap_parse(f, 7 + plen, &txn));
    gw_mblocal_execute(&cfg, &io, &txn, &wr);
    return gw_mbap_response(&txn, out, sizeof(out));
}

static void test_is_local(void)
{
    TEST_ASSERT_TRUE(gw_mblocal_is_local(&cfg, 100));
    TEST_ASSERT_FALSE(gw_mblocal_is_local(&cfg, 1));
    cfg.modbus_tcp.local_unit_id = 0;   /* off: no unit is local, also not 0 */
    TEST_ASSERT_FALSE(gw_mblocal_is_local(&cfg, 0));
    TEST_ASSERT_FALSE(gw_mblocal_is_local(&cfg, 100));
}

static void test_read_dis_and_dos(void)
{
    size_t n = run((const uint8_t[]){0x02, 0x00, 0x00, 0x00, 0x08}, 5);
    TEST_ASSERT_EQUAL(10, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x12, 0x34, 0, 0, 0, 4, 100, 0x02, 1, 0x05}), out, 10);
    /* DI/3 alone: address 2, one bit */
    run((const uint8_t[]){0x02, 0x00, 0x02, 0x00, 0x01}, 5);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[9]);
    n = run((const uint8_t[]){0x01, 0x00, 0x01, 0x00, 0x07}, 5);   /* DO/2 to DO/8 */
    TEST_ASSERT_EQUAL(10, n);
    TEST_ASSERT_EQUAL_HEX8(0x41, out[9]);   /* DO/2 -> bit 0, DO/8 -> bit 6 */
}

static void test_read_counters(void)
{
    size_t n = run((const uint8_t[]){0x04, 0x00, 0x00, 0x00, 0x04}, 5);
    TEST_ASSERT_EQUAL(9 + 8, n);
    TEST_ASSERT_EQUAL_HEX8(8, out[8]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){1, 2, 3, 4, 5, 6, 7, 8}), &out[9], 8);
    /* the last register of DI/8's counter: the least significant word of 42 */
    run((const uint8_t[]){0x04, 0x00, 0x1F, 0x00, 0x01}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x00, 0x2A}), &out[9], 2);
    /* all 32 registers in one request */
    n = run((const uint8_t[]){0x04, 0x00, 0x00, 0x00, 0x20}, 5);
    TEST_ASSERT_EQUAL(9 + 64, n);
}

static void test_addresses_outside_the_map(void)
{
    run((const uint8_t[]){0x02, 0x00, 0x01, 0x00, 0x08}, 5);   /* 1..8: DI/9 does not exist */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x82, 0x02}), &out[7], 2);
    run((const uint8_t[]){0x01, 0x00, 0x08, 0x00, 0x01}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x81, 0x02}), &out[7], 2);
    run((const uint8_t[]){0x04, 0x00, 0x1E, 0x00, 0x03}, 5);   /* 30..32 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x84, 0x02}), &out[7], 2);
    /* no holding registers */
    run((const uint8_t[]){0x03, 0x00, 0x00, 0x00, 0x01}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x83, 0x01}), &out[7], 2);
    run((const uint8_t[]){0x06, 0x00, 0x00, 0x00, 0x01}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x86, 0x01}), &out[7], 2);
    TEST_ASSERT_EQUAL_UINT8(0, wr.mask);
}

static void test_writes_need_local_writes(void)
{
    run((const uint8_t[]){0x05, 0x00, 0x00, 0xFF, 0x00}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x85, 0x01}), &out[7], 2);
    TEST_ASSERT_EQUAL_UINT8(0, wr.mask);
    run((const uint8_t[]){0x0F, 0x00, 0x00, 0x00, 0x02, 0x01, 0x03}, 7);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x8F, 0x01}), &out[7], 2);
    TEST_ASSERT_EQUAL_UINT8(0, wr.mask);
}

static void test_writes(void)
{
    cfg.modbus_tcp.local_writes = true;
    size_t n = run((const uint8_t[]){0x05, 0x00, 0x03, 0xFF, 0x00}, 5);   /* DO/4 on */
    TEST_ASSERT_EQUAL_UINT8(0x08, wr.mask);
    TEST_ASSERT_EQUAL_UINT8(0x08, wr.value);
    TEST_ASSERT_EQUAL(12, n);   /* the echo of the request */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x05, 0x00, 0x03, 0xFF, 0x00}), &out[7], 5);
    run((const uint8_t[]){0x05, 0x00, 0x01, 0x00, 0x00}, 5);   /* DO/2 off */
    TEST_ASSERT_EQUAL_UINT8(0x02, wr.mask);
    TEST_ASSERT_EQUAL_UINT8(0x00, wr.value);
    /* FC15: DO/5..DO/7 = on, off, on */
    n = run((const uint8_t[]){0x0F, 0x00, 0x04, 0x00, 0x03, 0x01, 0x05}, 7);
    TEST_ASSERT_EQUAL_UINT8(0x70, wr.mask);
    TEST_ASSERT_EQUAL_UINT8(0x50, wr.value);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x0F, 0x00, 0x04, 0x00, 0x03}), &out[7], 5);
    /* a write past DO/8 */
    run((const uint8_t[]){0x0F, 0x00, 0x07, 0x00, 0x02, 0x01, 0x03}, 7);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x8F, 0x02}), &out[7], 2);
    TEST_ASSERT_EQUAL_UINT8(0, wr.mask);
    /* the local_io task did not apply it */
    run((const uint8_t[]){0x05, 0x00, 0x00, 0xFF, 0x00}, 5);
    gw_mblocal_write_failed(&txn);
    gw_mbap_response(&txn, out, sizeof(out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x85, 0x04}), &out[7], 2);
}

/* IO-7: a DO that a local rule drives refuses the write, as for an NCMD; nothing changes */
static void test_rule_and_unknown_dos(void)
{
    cfg.modbus_tcp.local_writes = true;
    io.rule_do = 0x10;   /* DO/5 */
    run((const uint8_t[]){0x0F, 0x00, 0x03, 0x00, 0x03, 0x01, 0x07}, 7);   /* DO/4..DO/6 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x8F, 0x03}), &out[7], 2);
    TEST_ASSERT_EQUAL_UINT8(0, wr.mask);
    run((const uint8_t[]){0x05, 0x00, 0x03, 0xFF, 0x00}, 5);   /* DO/4 is free */
    TEST_ASSERT_EQUAL_UINT8(0x08, wr.mask);
    /* the expander does not answer: the DOs are unknown */
    io.do_known = false;
    run((const uint8_t[]){0x01, 0x00, 0x00, 0x00, 0x08}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x81, 0x04}), &out[7], 2);
    run((const uint8_t[]){0x05, 0x00, 0x03, 0xFF, 0x00}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x85, 0x04}), &out[7], 2);
    TEST_ASSERT_EQUAL_UINT8(0, wr.mask);
    /* the DIs and the counters stay readable */
    run((const uint8_t[]){0x02, 0x00, 0x00, 0x00, 0x08}, 5);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[7]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_is_local);
    RUN_TEST(test_read_dis_and_dos);
    RUN_TEST(test_read_counters);
    RUN_TEST(test_addresses_outside_the_map);
    RUN_TEST(test_writes_need_local_writes);
    RUN_TEST(test_writes);
    RUN_TEST(test_rule_and_unknown_dos);
    return UNITY_END();
}
