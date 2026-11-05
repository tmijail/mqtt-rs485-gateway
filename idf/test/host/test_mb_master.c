/* QA-1: Modbus master transactions (timing, retries, error handling) against a simulated slave. */
#include "mb_master.h"
#include "support.h"
#include "unity.h"

static sim_slave_t s;
static mb_master_t m;

static void init(uint32_t baud, uint8_t retries, uint16_t ifd_ms)
{
    sim_reset(&s, 1);
    mb_transport_t t = sim_transport(&s);
    mb_master_cfg_t c = {.baud = baud, .response_timeout_ms = 200, .retries = retries,
                         .inter_frame_delay_ms = ifd_ms};
    mb_master_init(&m, &t, &c);
}

void setUp(void)
{
    init(9600, 2, 5);
}
void tearDown(void) {}

static void test_timing_constants(void)
{
    TEST_ASSERT_EQUAL(1146, m.char_us);          /* 11 bits at 9600 baud */
    TEST_ASSERT_EQUAL(4011, m.t35_us);
    init(115200, 0, 0);
    TEST_ASSERT_EQUAL(1750, m.t35_us);           /* fixed above 19200 baud */
}

/* Issue #2: the wait for the end of a transmission must cover the whole frame. */
static void test_tx_timeout_covers_the_frame(void)
{
    TEST_ASSERT_EQUAL(10, mb_char_bits(false, 1));   /* 8N1 */
    TEST_ASSERT_EQUAL(11, mb_char_bits(true, 1));    /* 8E1 / 8O1 */
    TEST_ASSERT_EQUAL(11, mb_char_bits(false, 2));   /* 8N2 */
    TEST_ASSERT_EQUAL(12, mb_char_bits(true, 2));    /* 8E2 */

    /* FC16 of 2 registers (one 32-bit value): 7 + 4 data + 2 CRC = 13 bytes */
    mb_request_t rq = {.unit = 1, .fc = MB_FC_WRITE_MULTIPLE_REGISTERS, .addr = 0, .qty = 2,
                       .regs = (const uint16_t[]){1, 2}};
    uint8_t adu[MB_ADU_MAX];
    size_t len = mb_build_request(&rq, adu, sizeof(adu));
    TEST_ASSERT_EQUAL(13, len);
    /* 1200 baud with parity: 13 x 11 bits = 119.2 ms on the wire, more than the old fixed 100 ms */
    uint32_t wire_us = (uint32_t)(len * 11u * 1000000u / 1200u);
    TEST_ASSERT_EQUAL(119166, wire_us);
    uint32_t t = mb_tx_timeout_ms(1200, mb_char_bits(true, 1), len);
    TEST_ASSERT_GREATER_THAN_UINT32(wire_us / 1000u + 1u, t);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(wire_us / 1000u + 1u + 20u, t);
    /* 2 stop bits too: 13 x 12 bits = 130 ms */
    TEST_ASSERT_GREATER_THAN_UINT32(130, mb_tx_timeout_ms(1200, mb_char_bits(true, 2), len));

    /* longest FC16 the protocol allows (123 registers, 255 bytes) at the slowest setting:
     * covered, and still far below the 15 s task watchdog */
    uint16_t regs[MB_MAX_WRITE_REGS] = {0};
    rq.qty = MB_MAX_WRITE_REGS;
    rq.regs = regs;
    len = mb_build_request(&rq, adu, sizeof(adu));
    TEST_ASSERT_EQUAL(255, len);
    t = mb_tx_timeout_ms(1200, mb_char_bits(true, 2), len);
    TEST_ASSERT_GREATER_THAN_UINT32(2550, t);         /* 255 x 12 bits = 2550 ms */
    TEST_ASSERT_LESS_THAN_UINT32(3000, t);

    /* fast links keep a short bound */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(22, mb_tx_timeout_ms(115200, 10, 13));
}

static void test_read_ok_and_silence(void)
{
    s.holding[10] = 0xBEEF;
    s.clock_us = 1000000;  /* the bus has been idle for a long time: no wait on the first frame */
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_HOLDING_REGISTERS, .addr = 10, .qty = 1};
    mb_response_t rsp;
    TEST_ASSERT_EQUAL(MB_OK, mb_master_execute(&m, &rq, &rsp));
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, mb_rsp_reg(&rsp, 0));
    TEST_ASSERT_EQUAL(0, s.delay_total_us);
    /* back-to-back transaction: t3.5 + inter-frame delay must elapse after the last frame */
    TEST_ASSERT_EQUAL(MB_OK, mb_master_execute(&m, &rq, &rsp));
    TEST_ASSERT_EQUAL(4011 + 5000, s.delay_total_us);
    TEST_ASSERT_EQUAL(2, m.stats.ok);
}

static void test_timeout_with_retries(void)
{
    s.offline = true;
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_INPUT_REGISTERS, .addr = 0, .qty = 2};
    mb_response_t rsp;
    TEST_ASSERT_EQUAL(MB_ERR_TIMEOUT, mb_master_execute(&m, &rq, &rsp));
    TEST_ASSERT_EQUAL(3, s.requests);            /* 1 + 2 retries */
    TEST_ASSERT_EQUAL(3, m.stats.timeout);
}

static void test_crc_error_is_retried(void)
{
    s.input[0] = 42;
    s.crc_errors = 2;
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_INPUT_REGISTERS, .addr = 0, .qty = 1};
    mb_response_t rsp;
    TEST_ASSERT_EQUAL(MB_OK, mb_master_execute(&m, &rq, &rsp));
    TEST_ASSERT_EQUAL(42, mb_rsp_reg(&rsp, 0));
    TEST_ASSERT_EQUAL(3, s.requests);
    TEST_ASSERT_EQUAL(2, m.stats.crc);
}

static void test_wrong_unit_is_frame_error(void)
{
    s.wrong_unit = 5;
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_COILS, .addr = 0, .qty = 1};
    mb_response_t rsp;
    TEST_ASSERT_EQUAL(MB_ERR_FRAME, mb_master_execute(&m, &rq, &rsp));
    TEST_ASSERT_EQUAL(3, m.stats.frame);
}

static void test_exception_not_retried(void)
{
    s.exception = 2;
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_HOLDING_REGISTERS, .addr = 0, .qty = 1};
    mb_response_t rsp;
    TEST_ASSERT_EQUAL(MB_ERR_EXCEPTION, mb_master_execute(&m, &rq, &rsp));
    TEST_ASSERT_EQUAL(2, rsp.exception);
    TEST_ASSERT_EQUAL(1, s.requests);
    TEST_ASSERT_EQUAL(1, m.stats.exception);
}

static void test_writes(void)
{
    mb_response_t rsp;
    mb_request_t w6 = {.unit = 1, .fc = MB_FC_WRITE_SINGLE_REGISTER, .addr = 10, .value = 123};
    TEST_ASSERT_EQUAL(MB_OK, mb_master_execute(&m, &w6, &rsp));
    TEST_ASSERT_EQUAL(123, s.holding[10]);
    mb_request_t w5 = {.unit = 1, .fc = MB_FC_WRITE_SINGLE_COIL, .addr = 3, .value = 1};
    TEST_ASSERT_EQUAL(MB_OK, mb_master_execute(&m, &w5, &rsp));
    TEST_ASSERT_TRUE(s.coils[3]);
    const uint8_t bits[1] = {0x05};
    mb_request_t w15 = {.unit = 1, .fc = MB_FC_WRITE_MULTIPLE_COILS, .addr = 8, .qty = 3,
                        .bits = bits};
    TEST_ASSERT_EQUAL(MB_OK, mb_master_execute(&m, &w15, &rsp));
    TEST_ASSERT_TRUE(s.coils[8]);
    TEST_ASSERT_FALSE(s.coils[9]);
    TEST_ASSERT_TRUE(s.coils[10]);
    const uint16_t regs[2] = {0x4144, 0xCCCD};
    mb_request_t w16 = {.unit = 1, .fc = MB_FC_WRITE_MULTIPLE_REGISTERS, .addr = 20, .qty = 2,
                        .regs = regs};
    TEST_ASSERT_EQUAL(MB_OK, mb_master_execute(&m, &w16, &rsp));
    TEST_ASSERT_EQUAL_HEX16(0x4144, s.holding[20]);
    TEST_ASSERT_EQUAL_HEX16(0xCCCD, s.holding[21]);
}

/* Issue #112: the bus task can stop between two attempts (on_attempt) in the QEMU build; the
 * park of core 1 waits that long at most. Worst valid configuration: 1200 baud, 5 s response
 * timeout, 5 retries, 1 s inter-frame delay, so a whole transaction lasts over 30 s. */
static uint64_t s_attempt_at[8];
static unsigned s_attempts;

static void record_attempt(void *ctx)
{
    if (s_attempts < 8) {
        s_attempt_at[s_attempts] = ((sim_slave_t *)ctx)->clock_us;
    }
    s_attempts++;
}

static void test_attempt_bound(void)
{
    /* default settings: t3.5 4.011 ms + 5 + tx 256 B 12-bit at 9600 (320 + 20) + 200 + frame
     * timeouts of 1 and 256 characters (21 + 297) */
    uint32_t b = mb_master_attempt_max_ms(&m);
    TEST_ASSERT_EQUAL_UINT32(5 + 5 + 340 + 200 + 25 + 317, b);

    sim_reset(&s, 1);
    s.offline = true;
    mb_transport_t t = sim_transport(&s);
    t.on_attempt = record_attempt;
    mb_master_cfg_t c = {.baud = 1200, .response_timeout_ms = 5000, .retries = 5,
                         .inter_frame_delay_ms = 1000};
    mb_master_init(&m, &t, &c);
    b = mb_master_attempt_max_ms(&m);
    TEST_ASSERT_GREATER_THAN_UINT32(10000, b);   /* the old fixed wait refused this one */
    TEST_ASSERT_LESS_THAN_UINT32(15000, b);      /* below the 15 s task watchdog */

    s_attempts = 0;
    s.clock_us = 1000000;
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_HOLDING_REGISTERS, .addr = 0, .qty = 10};
    mb_response_t rsp;
    TEST_ASSERT_EQUAL(MB_ERR_TIMEOUT, mb_master_execute(&m, &rq, &rsp));
    uint64_t end = s.clock_us;
    TEST_ASSERT_EQUAL(6, s_attempts);
    TEST_ASSERT_GREATER_THAN_UINT64(30000000u, end - s_attempt_at[0]);
    for (unsigned i = 1; i < 6; i++) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT64((uint64_t)b * 1000u, s_attempt_at[i] - s_attempt_at[i - 1]);
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT64((uint64_t)b * 1000u, end - s_attempt_at[5]);
}

static void test_invalid_request(void)
{
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_HOLDING_REGISTERS, .addr = 0, .qty = 0};
    mb_response_t rsp;
    TEST_ASSERT_EQUAL(MB_ERR_ARG, mb_master_execute(&m, &rq, &rsp));
    TEST_ASSERT_EQUAL(0, s.requests);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_timing_constants);
    RUN_TEST(test_tx_timeout_covers_the_frame);
    RUN_TEST(test_read_ok_and_silence);
    RUN_TEST(test_timeout_with_retries);
    RUN_TEST(test_crc_error_is_retried);
    RUN_TEST(test_wrong_unit_is_frame_error);
    RUN_TEST(test_exception_not_retried);
    RUN_TEST(test_writes);
    RUN_TEST(test_attempt_bound);
    RUN_TEST(test_invalid_request);
    return UNITY_END();
}
