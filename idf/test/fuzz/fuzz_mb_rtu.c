/*
 * Issue #226: the RS485 bus. A response ADU from a Modbus RTU slave (or from noise on the bus)
 * against a request: mb_response_length() byte by byte as the master receives it,
 * mb_parse_response(), the accessors of the parsed data, and the whole master transaction
 * (mb_master_execute) on a transport that receives the input.
 *
 * Input: a selector byte (bits 0-2: the function code; bit 7: the whole transaction through
 * mb_master_execute, else the parser byte for byte), the unit, the address (2 bytes), the quantity
 * (2 bytes), the value (2 bytes), the retries, then the bytes on the bus.
 */
#include "fuzz.h"
#include "mb_master.h"
#include "mb_rtu.h"

static const mb_fc_t k_fc[8] = {
    MB_FC_READ_COILS,           MB_FC_READ_DISCRETE_INPUTS, MB_FC_READ_HOLDING_REGISTERS,
    MB_FC_READ_INPUT_REGISTERS, MB_FC_WRITE_SINGLE_COIL,    MB_FC_WRITE_SINGLE_REGISTER,
    MB_FC_WRITE_MULTIPLE_COILS, MB_FC_WRITE_MULTIPLE_REGISTERS,
};

static uint16_t s_regs[MB_MAX_WRITE_REGS];
static uint8_t s_bits[(MB_MAX_WRITE_BITS + 7) / 8];

/* The data a parsed response gives to its users (gw_poll): every value inside the request. */
static void use_response(const mb_request_t *rq, const mb_response_t *rsp)
{
    volatile unsigned sink = 0;
    switch (rq->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
        FZ_CHECK(rsp->byte_count == (rq->qty + 7u) / 8u);
        for (uint16_t i = 0; i < rq->qty; i++) {
            sink += mb_rsp_bit(rsp, i);
        }
        break;
    case MB_FC_READ_HOLDING_REGISTERS:
    case MB_FC_READ_INPUT_REGISTERS:
        FZ_CHECK(rsp->byte_count == 2u * rq->qty);
        for (uint16_t i = 0; i < rq->qty; i++) {
            sink += mb_rsp_reg(rsp, i);
        }
        break;
    default:
        break;
    }
    (void)sink;
}

/* ---- a transport that receives the rest of the input ---- */

typedef struct {
    fz_t in;
    uint64_t now_us;
} bus_t;

static int t_write(void *ctx, const uint8_t *d, size_t len)
{
    bus_t *b = ctx;
    (void)d;
    b->now_us += 100u * len;
    return 0;
}

static int t_read(void *ctx, uint8_t *d, size_t len, uint32_t timeout_ms)
{
    bus_t *b = ctx;
    size_t n;
    const uint8_t *p = fz_bytes(&b->in, len, &n);
    memcpy(d, p, n);
    b->now_us += n != 0 ? 100u * n : 1000u * timeout_ms;
    return (int)n;
}

static void t_flush(void *ctx)
{
    (void)ctx;
}

static void t_delay(void *ctx, uint32_t us)
{
    bus_t *b = ctx;
    b->now_us += us;
}

static uint64_t t_now(void *ctx)
{
    bus_t *b = ctx;
    return b->now_us;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t sel = fz_u8(&f);
    mb_request_t rq = {
        .unit = fz_u8(&f),
        .fc = k_fc[sel & 7u],
        .addr = fz_u16(&f),
        .qty = fz_u16(&f),
        .value = fz_u16(&f),
        .regs = s_regs,
        .bits = s_bits,
    };
    uint8_t retries = fz_u8(&f) & 3u;

    uint8_t adu[MB_ADU_MAX];
    size_t req_len = mb_build_request(&rq, adu, sizeof(adu));
    FZ_CHECK(req_len <= sizeof(adu));
    if (req_len == 0) {
        return 0;   /* the master sends no request that it cannot build (MB_ERR_ARG) */
    }

    if ((sel & 0x80u) != 0) {
        /* the master, as the rs485_bus task runs it */
        bus_t bus = {.in = f};
        mb_transport_t tr = {t_write, t_read, t_flush, t_delay, t_now, NULL, &bus};
        mb_master_cfg_t mc = {.baud = 9600, .response_timeout_ms = 200, .retries = retries,
                              .inter_frame_delay_ms = 5};
        mb_master_t m;
        mb_master_init(&m, &tr, &mc);
        mb_response_t rsp;
        mb_status_t st = mb_master_execute(&m, &rq, &rsp);
        if (st == MB_OK) {
            use_response(&rq, &rsp);
        }
        return 0;
    }

    size_t len;
    const uint8_t *rest = fz_rest(&f, &len);
    uint8_t *buf = fz_dup(rest, len);
    /* byte by byte, as the bytes arrive */
    for (size_t have = 0; have <= len; have++) {
        size_t need = mb_response_length(&rq, buf, have);
        if (need == SIZE_MAX) {
            break;
        }
        if (need != 0 && need <= have) {
            mb_response_t rsp;
            if (mb_parse_response(&rq, buf, need, &rsp) == MB_OK) {
                use_response(&rq, &rsp);
            }
            break;
        }
    }
    /* the whole buffer as one frame */
    mb_response_t rsp;
    if (mb_parse_response(&rq, buf, len, &rsp) == MB_OK) {
        use_response(&rq, &rsp);
    }
    free(buf);
    return 0;
}
