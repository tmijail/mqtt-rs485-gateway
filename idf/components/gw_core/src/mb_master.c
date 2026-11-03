#include "mb_master.h"

#include <string.h>

#include "gw_port.h"

static const char *TAG = "mb";

/* Margin added to computed frame times to absorb scheduler and UART RX-timeout latency. */
#define MB_FRAME_MARGIN_MS 20u

void mb_master_init(mb_master_t *m, const mb_transport_t *tr, const mb_master_cfg_t *cfg)
{
    memset(m, 0, sizeof(*m));
    m->tr = *tr;
    m->cfg = *cfg;
    /* Modbus over serial line spec: 11 bits per character in RTU mode. */
    m->char_us = (11u * 1000000u + cfg->baud - 1u) / cfg->baud;
    m->t35_us = cfg->baud > 19200u ? 1750u : (m->char_us * 7u + 1u) / 2u;
}

static uint32_t frame_timeout_ms(const mb_master_t *m, size_t nbytes)
{
    return (uint32_t)((nbytes * m->char_us + m->t35_us) / 1000u) + MB_FRAME_MARGIN_MS;
}

static void wait_bus_silence(mb_master_t *m)
{
    uint64_t ready = m->last_bus_us + m->t35_us + (uint64_t)m->cfg.inter_frame_delay_ms * 1000u;
    uint64_t now = m->tr.now_us(m->tr.ctx);
    if (now < ready) {
        m->tr.delay_us(m->tr.ctx, (uint32_t)(ready - now));
    }
}

static int rx(mb_master_t *m, size_t off, size_t len, uint32_t timeout_ms)
{
    int n = m->tr.read(m->tr.ctx, &m->rx[off], len, timeout_ms);
    return n < 0 ? 0 : n;
}

static mb_status_t attempt(mb_master_t *m, const mb_request_t *rq, size_t tx_len,
                           mb_response_t *rsp)
{
    wait_bus_silence(m);
    m->tr.flush(m->tr.ctx);
    int werr = m->tr.write(m->tr.ctx, m->tx, tx_len);
    m->last_bus_us = m->tr.now_us(m->tr.ctx);
    if (werr != 0) {
        return MB_ERR_FRAME;
    }

    size_t have = (size_t)rx(m, 0, 2, m->cfg.response_timeout_ms);
    if (have == 0) {
        return MB_ERR_TIMEOUT;
    }
    size_t need = have < 2 ? SIZE_MAX : mb_response_length(rq, m->rx, have);
    if (need == 0) {
        have += (size_t)rx(m, have, 1, frame_timeout_ms(m, 1));
        need = mb_response_length(rq, m->rx, have);
    }
    if (need == 0 || need == SIZE_MAX || need > sizeof(m->rx)) {
        /* Not a response to this request: drain the rest of the frame before giving up. */
        rx(m, have, sizeof(m->rx) - have, frame_timeout_ms(m, 8));
        m->last_bus_us = m->tr.now_us(m->tr.ctx);
        return MB_ERR_FRAME;
    }
    if (need > have) {
        have += (size_t)rx(m, have, need - have, frame_timeout_ms(m, need - have));
    }
    m->last_bus_us = m->tr.now_us(m->tr.ctx);
    if (have < need) {
        return MB_ERR_FRAME;
    }
    return mb_parse_response(rq, m->rx, have, rsp);
}

mb_status_t mb_master_execute(mb_master_t *m, const mb_request_t *rq, mb_response_t *rsp)
{
    size_t tx_len = mb_build_request(rq, m->tx, sizeof(m->tx));
    if (tx_len == 0) {
        return MB_ERR_ARG;
    }
    mb_status_t st = MB_ERR_TIMEOUT;
    for (unsigned i = 0; i <= m->cfg.retries; i++) {
        if (m->tr.on_attempt != NULL) {
            m->tr.on_attempt(m->tr.ctx);
        }
        st = attempt(m, rq, tx_len, rsp);
        switch (st) {
        case MB_OK:
            m->stats.ok++;
            return st;
        case MB_ERR_EXCEPTION:
            m->stats.exception++;
            GW_LOGD(TAG, "unit %u fc %u addr %u: exception %u", rq->unit, rq->fc, rq->addr,
                    rsp->exception);
            return st;
        case MB_ERR_TIMEOUT:
            m->stats.timeout++;
            break;
        case MB_ERR_CRC:
            m->stats.crc++;
            break;
        default:
            m->stats.frame++;
            break;
        }
        GW_LOGD(TAG, "unit %u fc %u addr %u: attempt %u failed (%d)", rq->unit, rq->fc, rq->addr,
                i + 1, st);
    }
    return st;
}
