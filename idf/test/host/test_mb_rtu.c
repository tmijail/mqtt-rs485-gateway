/* QA-1: Modbus RTU CRC and framing. */
#include <string.h>

#include "mb_rtu.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_crc_known_vectors(void)
{
    /* Read holding registers 0..9 of unit 1: canonical frame 01 03 00 00 00 0A C5 CD. */
    const uint8_t f[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A};
    TEST_ASSERT_EQUAL_HEX16(0xCDC5, mb_crc16(f, sizeof(f)));
    const uint8_t g[] = {0x11, 0x03, 0x00, 0x6B, 0x00, 0x03}; /* Modbus spec example: CRC 76 87 */
    TEST_ASSERT_EQUAL_HEX16(0x8776, mb_crc16(g, sizeof(g)));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, mb_crc16(f, 0));
}

static void test_build_read(void)
{
    uint8_t adu[MB_ADU_MAX];
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_HOLDING_REGISTERS, .addr = 0, .qty = 10};
    TEST_ASSERT_EQUAL(8, mb_build_request(&rq, adu, sizeof(adu)));
    const uint8_t exp[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp, adu, 8);

    rq.fc = MB_FC_READ_COILS;
    rq.qty = 2001;
    TEST_ASSERT_EQUAL(0, mb_build_request(&rq, adu, sizeof(adu)));
    rq.fc = MB_FC_READ_INPUT_REGISTERS;
    rq.qty = 126;
    TEST_ASSERT_EQUAL(0, mb_build_request(&rq, adu, sizeof(adu)));
    rq.qty = 2;
    rq.addr = 0xFFFF;
    TEST_ASSERT_EQUAL(0, mb_build_request(&rq, adu, sizeof(adu)));
    rq.addr = 0;
    rq.unit = 0;  /* broadcast is not used by a polling master */
    TEST_ASSERT_EQUAL(0, mb_build_request(&rq, adu, sizeof(adu)));
}

static void test_build_writes(void)
{
    uint8_t adu[MB_ADU_MAX];
    mb_request_t c = {.unit = 1, .fc = MB_FC_WRITE_SINGLE_COIL, .addr = 0x00AC, .value = 1};
    TEST_ASSERT_EQUAL(8, mb_build_request(&c, adu, sizeof(adu)));
    const uint8_t exp5[] = {0x01, 0x05, 0x00, 0xAC, 0xFF, 0x00};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp5, adu, 6);

    mb_request_t r = {.unit = 1, .fc = MB_FC_WRITE_SINGLE_REGISTER, .addr = 10, .value = 123};
    TEST_ASSERT_EQUAL(8, mb_build_request(&r, adu, sizeof(adu)));
    const uint8_t exp6[] = {0x01, 0x06, 0x00, 0x0A, 0x00, 0x7B};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp6, adu, 6);

    const uint16_t regs[2] = {0x4144, 0xCCCD};
    mb_request_t m = {.unit = 7, .fc = MB_FC_WRITE_MULTIPLE_REGISTERS, .addr = 4, .qty = 2,
                      .regs = regs};
    TEST_ASSERT_EQUAL(13, mb_build_request(&m, adu, sizeof(adu)));
    const uint8_t exp16[] = {0x07, 0x10, 0x00, 0x04, 0x00, 0x02, 0x04, 0x41, 0x44, 0xCC, 0xCD};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp16, adu, 11);
    TEST_ASSERT_EQUAL_HEX16(mb_crc16(adu, 11), (uint16_t)(adu[11] | (adu[12] << 8)));

    const uint8_t bits[2] = {0xCD, 0xFF};  /* 10 coils: only the low 2 bits of byte 2 survive */
    mb_request_t mc = {.unit = 1, .fc = MB_FC_WRITE_MULTIPLE_COILS, .addr = 19, .qty = 10,
                       .bits = bits};
    TEST_ASSERT_EQUAL(11, mb_build_request(&mc, adu, sizeof(adu)));
    const uint8_t exp15[] = {0x01, 0x0F, 0x00, 0x13, 0x00, 0x0A, 0x02, 0xCD, 0x03};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(exp15, adu, 9);

    m.qty = 124;
    TEST_ASSERT_EQUAL(0, mb_build_request(&m, adu, sizeof(adu)));
}

static size_t with_crc(uint8_t *f, size_t n)
{
    uint16_t crc = mb_crc16(f, n);
    f[n] = (uint8_t)crc;
    f[n + 1] = (uint8_t)(crc >> 8);
    return n + 2;
}

static void test_response_length(void)
{
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_HOLDING_REGISTERS, .addr = 0, .qty = 2};
    uint8_t h[3] = {0x01, 0x03, 0x04};
    TEST_ASSERT_EQUAL(0, mb_response_length(&rq, h, 1));
    TEST_ASSERT_EQUAL(0, mb_response_length(&rq, h, 2));
    TEST_ASSERT_EQUAL(9, mb_response_length(&rq, h, 3));
    uint8_t e[2] = {0x01, 0x83};
    TEST_ASSERT_EQUAL(5, mb_response_length(&rq, e, 2));
    uint8_t wrong_unit[2] = {0x02, 0x03};
    TEST_ASSERT_EQUAL(SIZE_MAX, mb_response_length(&rq, wrong_unit, 2));
    uint8_t wrong_fc[2] = {0x01, 0x04};
    TEST_ASSERT_EQUAL(SIZE_MAX, mb_response_length(&rq, wrong_fc, 2));
    mb_request_t wr = {.unit = 1, .fc = MB_FC_WRITE_SINGLE_REGISTER, .addr = 0, .value = 1};
    uint8_t w[2] = {0x01, 0x06};
    TEST_ASSERT_EQUAL(8, mb_response_length(&wr, w, 2));
}

static void test_parse_read_responses(void)
{
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_HOLDING_REGISTERS, .addr = 0, .qty = 2};
    uint8_t f[16] = {0x01, 0x03, 0x04, 0x12, 0x34, 0xAB, 0xCD};
    size_t n = with_crc(f, 7);
    mb_response_t rsp;
    TEST_ASSERT_EQUAL(MB_OK, mb_parse_response(&rq, f, n, &rsp));
    TEST_ASSERT_EQUAL_HEX16(0x1234, mb_rsp_reg(&rsp, 0));
    TEST_ASSERT_EQUAL_HEX16(0xABCD, mb_rsp_reg(&rsp, 1));

    f[n - 1] ^= 1;
    TEST_ASSERT_EQUAL(MB_ERR_CRC, mb_parse_response(&rq, f, n, &rsp));
    TEST_ASSERT_EQUAL(MB_ERR_FRAME, mb_parse_response(&rq, f, n - 1, &rsp));

    /* byte count inconsistent with the requested quantity */
    uint8_t g[16] = {0x01, 0x03, 0x02, 0x12, 0x34};
    n = with_crc(g, 5);
    TEST_ASSERT_EQUAL(MB_ERR_FRAME, mb_parse_response(&rq, g, n, &rsp));

    mb_request_t bits = {.unit = 1, .fc = MB_FC_READ_COILS, .addr = 0, .qty = 10};
    uint8_t b[16] = {0x01, 0x01, 0x02, 0b10000101, 0b00000010};
    n = with_crc(b, 5);
    TEST_ASSERT_EQUAL(MB_OK, mb_parse_response(&bits, b, n, &rsp));
    TEST_ASSERT_TRUE(mb_rsp_bit(&rsp, 0));
    TEST_ASSERT_FALSE(mb_rsp_bit(&rsp, 1));
    TEST_ASSERT_TRUE(mb_rsp_bit(&rsp, 2));
    TEST_ASSERT_TRUE(mb_rsp_bit(&rsp, 7));
    TEST_ASSERT_FALSE(mb_rsp_bit(&rsp, 8));
    TEST_ASSERT_TRUE(mb_rsp_bit(&rsp, 9));
}

static void test_parse_exception_and_echo(void)
{
    mb_request_t rq = {.unit = 1, .fc = MB_FC_READ_INPUT_REGISTERS, .addr = 0, .qty = 1};
    uint8_t f[8] = {0x01, 0x84, 0x02};
    size_t n = with_crc(f, 3);
    mb_response_t rsp;
    TEST_ASSERT_EQUAL(MB_ERR_EXCEPTION, mb_parse_response(&rq, f, n, &rsp));
    TEST_ASSERT_EQUAL(2, rsp.exception);

    mb_request_t wr = {.unit = 1, .fc = MB_FC_WRITE_SINGLE_REGISTER, .addr = 10, .value = 123};
    uint8_t e[8] = {0x01, 0x06, 0x00, 0x0A, 0x00, 0x7B};
    n = with_crc(e, 6);
    TEST_ASSERT_EQUAL(MB_OK, mb_parse_response(&wr, e, n, &rsp));
    e[5] = 0x7C; /* echo with a different value */
    n = with_crc(e, 6);
    TEST_ASSERT_EQUAL(MB_ERR_FRAME, mb_parse_response(&wr, e, n, &rsp));

    const uint16_t regs[2] = {1, 2};
    mb_request_t m = {.unit = 1, .fc = MB_FC_WRITE_MULTIPLE_REGISTERS, .addr = 4, .qty = 2,
                      .regs = regs};
    uint8_t r[8] = {0x01, 0x10, 0x00, 0x04, 0x00, 0x02};
    n = with_crc(r, 6);
    TEST_ASSERT_EQUAL(MB_OK, mb_parse_response(&m, r, n, &rsp));
    r[5] = 0x03;
    n = with_crc(r, 6);
    TEST_ASSERT_EQUAL(MB_ERR_FRAME, mb_parse_response(&m, r, n, &rsp));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_known_vectors);
    RUN_TEST(test_build_read);
    RUN_TEST(test_build_writes);
    RUN_TEST(test_response_length);
    RUN_TEST(test_parse_read_responses);
    RUN_TEST(test_parse_exception_and_echo);
    return UNITY_END();
}
