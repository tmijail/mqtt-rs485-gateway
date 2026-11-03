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
    RUN_TEST(test_read_ok_and_silence);
    RUN_TEST(test_timeout_with_retries);
    RUN_TEST(test_crc_error_is_retried);
    RUN_TEST(test_wrong_unit_is_frame_error);
    RUN_TEST(test_exception_not_retried);
    RUN_TEST(test_writes);
    RUN_TEST(test_invalid_request);
    return UNITY_END();
}
