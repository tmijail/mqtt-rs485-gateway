/*
 * Issue #226: Modbus TCP (MB-15). The byte stream of a TCP client, handled as gw_mbtcp does it:
 * gw_mbap_frame_len() on the receive buffer, gw_mbap_parse() of each frame, the gateway's own
 * exception (gw_mbap_exception), the local unit (gw_mblocal_execute, #216) or the RTU
 * transaction (gw_mbap_execute on a transport that receives the input), and the response
 * (gw_mbap_response).
 *
 * Input: the state of the local I/O (13 bytes), the length of the TCP stream (2 bytes), the
 * stream, then the bytes that the RTU slaves send.
 */
#include "fuzz.h"
#include "gw_config.h"
#include "gw_mbap.h"
#include "gw_mblocal.h"

static gw_config_t s_cfg;   /* tools/e2e/fs/config.json: the local unit 100, with writes */

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    size_t len;
    char *json = fz_read_file(GW_FUZZ_E2E_CONFIG, &len);
    gw_cfg_err_t err;
    if (!gw_config_parse(json, len, &s_cfg, &err)) {
        fprintf(stderr, "%s: %s\n", GW_FUZZ_E2E_CONFIG, err.msg);
        abort();
    }
    free(json);
    return 0;
}

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

static gw_mbap_txn_t s_txn;
static bool s_write_fails;   /* the local_io task does not apply a DO write */
static uint8_t s_out[GW_MBAP_ADU_MAX];

/* gw_mbtcp.c handle_frame(), without the queue to the bus owner. */
static void handle_frame(const uint8_t *frame, size_t len, const gw_mblocal_io_t *io,
                         mb_master_t *mb)
{
    bool local = gw_mblocal_is_local(&s_cfg, frame[6]);
    uint8_t code = gw_mbap_parse(frame, len, &s_txn);
    size_t n;
    if (code != 0) {
        n = gw_mbap_exception(frame, code, s_out, sizeof(s_out));
    } else if (local) {
        gw_mblocal_write_t wr;
        gw_mblocal_execute(&s_cfg, io, &s_txn, &wr);
        if (wr.mask != 0 && s_write_fails) {
            gw_mblocal_write_failed(&s_txn);
        }
        n = gw_mbap_response(&s_txn, s_out, sizeof(s_out));
    } else {
        gw_mbap_execute(mb, &s_txn);
        n = gw_mbap_response(&s_txn, s_out, sizeof(s_out));
    }
    FZ_CHECK(n <= sizeof(s_out));
    if (n != 0) {
        /* an answer is a well-formed MBAP frame with the transaction id of the request */
        FZ_CHECK(n >= GW_MBAP_HDR_LEN + 2);
        FZ_CHECK(gw_mbap_frame_len(s_out, n) == n);
        FZ_CHECK(s_out[0] == frame[0] && s_out[1] == frame[1]);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    gw_mblocal_io_t io = {.di = fz_u8(&f), .dout = fz_u8(&f)};
    uint8_t flags = fz_u8(&f);
    io.do_known = (flags & 1u) != 0;
    io.rule_do = fz_u8(&f);
    s_write_fails = (flags & 8u) != 0;
    for (size_t i = 0; i < GW_LOCAL_IO_CHANNELS; i++) {
        uint8_t b = fz_u8(&f);
        io.counters[i] = (flags & 2u) != 0 ? UINT64_MAX - b : b;
    }
    for (size_t i = 0; i < GW_RUN_HOURS_CHANNELS; i++) {
        io.run_s[i] = (flags & 4u) != 0 ? UINT64_MAX - i : 3600u * i;
    }
    size_t stream_len = fz_u16(&f);
    size_t got;
    const uint8_t *stream = fz_bytes(&f, stream_len, &got);

    bus_t bus = {.in = f};
    mb_transport_t tr = {t_write, t_read, t_flush, t_delay, t_now, NULL, &bus};
    mb_master_cfg_t mc = {.baud = 19200, .response_timeout_ms = 100, .retries = flags >> 6,
                          .inter_frame_delay_ms = 0};
    mb_master_t mb;
    mb_master_init(&mb, &tr, &mc);

    /* gw_mbtcp.c on_readable(): the receive buffer of a client, in pieces of up to 64 bytes */
    uint8_t *buf = malloc(GW_MBAP_ADU_MAX);
    if (buf == NULL) {
        abort();
    }
    size_t have = 0, pos = 0;
    while (pos < got) {
        size_t chunk = 1u + (stream[pos] & 63u);
        size_t room = GW_MBAP_ADU_MAX - have;
        if (chunk > got - pos) {
            chunk = got - pos;
        }
        if (chunk > room) {
            chunk = room;
        }
        memcpy(&buf[have], &stream[pos], chunk);
        have += chunk;
        pos += chunk;
        bool closed = false;
        for (;;) {
            size_t need = gw_mbap_frame_len(buf, have);
            if (need == SIZE_MAX) {
                closed = true;   /* the connection is dropped */
                break;
            }
            if (need == 0 || need > have) {
                FZ_CHECK(need <= GW_MBAP_ADU_MAX);   /* else the buffer can never hold it */
                break;
            }
            /* a copy of exactly the frame: a read past it is a finding */
            uint8_t *frame = fz_dup(buf, need);
            handle_frame(frame, need, &io, &mb);
            free(frame);
            memmove(buf, &buf[need], have - need);
            have -= need;
        }
        if (closed) {
            break;
        }
    }
    free(buf);
    return 0;
}
