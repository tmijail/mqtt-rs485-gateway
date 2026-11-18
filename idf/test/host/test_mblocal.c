/* #216: the local unit of the Modbus TCP server: the DIs (FC02), the DOs (FC01, FC05, FC15) and
 * the pulse counters (FC04, uint64 big) of the gateway, from a snapshot of its I/O. #234: the run
 * hours of DI/1..8 and DO/1..8 at FC04 32..95. */
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
    /* #234: run hours of DI/2, DO/1 and DO/8; DI/3 has a saved value but is not in the list */
    cfg.local_io.run_hours = (1u << 1) | (1u << 8) | (1u << 15);
    io.run_s[1] = 3600;
    io.run_s[2] = 777;
    io.run_s[8] = 0x0000000100000002ull;
    io.run_s[15] = 0xFEDCBA9876543210ull;
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

/* #234: FC04 32..63 the run hours of DI/1..8, 64..95 of DO/1..8 (uint64 s, big), the same value
 * as the metric Run Hours/<DI|DO>/n; 0 for a channel that is not in local_io.run_hours */
static void test_read_run_hours(void)
{
    size_t n = run((const uint8_t[]){0x04, 0x00, 0x20, 0x00, 0x40}, 5);   /* 32..95 */
    TEST_ASSERT_EQUAL(9 + 128, n);
    TEST_ASSERT_EQUAL_HEX8(128, out[8]);
    const uint8_t *d = &out[9];
    for (int k = 0; k < GW_RUN_HOURS_CHANNELS; k++) {
        uint64_t v = 0;
        for (int b = 0; b < 8; b++) {
            v = v << 8 | d[8 * k + b];
        }
        uint64_t want = k == 1 ? 3600u : k == 8 ? 0x0000000100000002ull
                                     : k == 15 ? 0xFEDCBA9876543210ull : 0u;
        TEST_ASSERT_EQUAL_HEX64(want, v);   /* k 2 (DI/3): not in the list -> 0, not 777 */
    }
    /* one register: the second word of DO/8 (register 64 + 7 * 4 + 1 = 93) */
    run((const uint8_t[]){0x04, 0x00, 0x5D, 0x00, 0x01}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0xBA, 0x98}), &out[9], 2);
    /* the last two registers (94, 95) are in the map */
    run((const uint8_t[]){0x04, 0x00, 0x5E, 0x00, 0x02}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x04, 0x76, 0x54, 0x32, 0x10}), &out[8], 5);
    /* a read across the counters and the run hours: 28..39 (DI/8's counter, DI/1, DI/2) */
    n = run((const uint8_t[]){0x04, 0x00, 0x1C, 0x00, 0x0C}, 5);
    TEST_ASSERT_EQUAL(9 + 24, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0, 0, 0, 0, 0, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                              0, 0, 0, 0, 0, 0x0E, 0x10}),
                                 &out[9], 24);
    /* the whole map in one request (96 registers) */
    n = run((const uint8_t[]){0x04, 0x00, 0x00, 0x00, 0x60}, 5);
    TEST_ASSERT_EQUAL(9 + 192, n);
}

static void test_addresses_outside_the_map(void)
{
    run((const uint8_t[]){0x02, 0x00, 0x01, 0x00, 0x08}, 5);   /* 1..8: DI/9 does not exist */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x82, 0x02}), &out[7], 2);
    run((const uint8_t[]){0x01, 0x00, 0x08, 0x00, 0x01}, 5);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x81, 0x02}), &out[7], 2);
    /* #234: the input registers end at 95; a range that crosses the end gives 02 as a whole */
    run((const uint8_t[]){0x04, 0x00, 0x60, 0x00, 0x01}, 5);   /* 96 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x84, 0x02}), &out[7], 2);
    run((const uint8_t[]){0x04, 0x00, 0x5E, 0x00, 0x03}, 5);   /* 94..96 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]){0x84, 0x02}), &out[7], 2);
    run((const uint8_t[]){0x04, 0x00, 0x00, 0x00, 0x61}, 5);   /* 0..96 */
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
    run((const uint8_t[]){0x0F, 0x00, 0x04, 0x00, 0x03, 0x01, 0x05}, 7);
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
    RUN_TEST(test_read_run_hours);
    RUN_TEST(test_addresses_outside_the_map);
    RUN_TEST(test_writes_need_local_writes);
    RUN_TEST(test_writes);
    RUN_TEST(test_rule_and_unknown_dos);
    return UNITY_END();
}
