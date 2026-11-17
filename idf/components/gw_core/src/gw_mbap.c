#include "gw_mbap.h"

#include <string.h>

#include "gw_port.h"

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

size_t gw_mbap_frame_len(const uint8_t *buf, size_t have)
{
    if (have < 6) {
        return 0;
    }
    uint16_t len = get16(&buf[4]);   /* unit id + PDU */
    if (get16(&buf[2]) != 0 || len < 2 || len > GW_MBAP_ADU_MAX - 6) {
        return SIZE_MAX;
    }
    return 6u + len;
}

uint8_t gw_mbap_parse(const uint8_t *frame, size_t len, gw_mbap_txn_t *txn)
{
    memset(txn, 0, sizeof(*txn));
    txn->tid = get16(frame);
    const uint8_t unit = frame[6];
    const uint8_t *pdu = &frame[GW_MBAP_HDR_LEN];
    size_t plen = len - GW_MBAP_HDR_LEN;
    uint8_t fc = pdu[0];
    mb_request_t *rq = &txn->rq;
    rq->unit = unit;
    rq->fc = (mb_fc_t)fc;
    bool known = (fc >= MB_FC_READ_COILS && fc <= MB_FC_WRITE_SINGLE_REGISTER) ||
                 fc == MB_FC_WRITE_MULTIPLE_COILS || fc == MB_FC_WRITE_MULTIPLE_REGISTERS;
    if (!known) {
        return GW_MBAP_EX_ILLEGAL_FUNCTION;
    }
    /* Unit 0 is the RTU broadcast (no answer) and 248..255 do not exist on a serial line;
     * many TCP clients send 255 meaning "the gateway itself", which has no data of its own. */
    if (unit == 0 || unit > 247) {
        return GW_MBAP_EX_PATH_UNAVAILABLE;
    }
    if (plen < 5) {
        return GW_MBAP_EX_ILLEGAL_VALUE;
    }
    rq->addr = get16(&pdu[1]);
    uint16_t v = get16(&pdu[3]);
    switch (fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
    case MB_FC_READ_HOLDING_REGISTERS:
    case MB_FC_READ_INPUT_REGISTERS: {
        uint16_t max = fc <= MB_FC_READ_DISCRETE_INPUTS ? MB_MAX_READ_BITS : MB_MAX_READ_REGS;
        if (plen != 5 || v == 0 || v > max || (uint32_t)rq->addr + v > 0x10000u) {
            return GW_MBAP_EX_ILLEGAL_VALUE;
        }
        rq->qty = v;
        return 0;
    }
    case MB_FC_WRITE_SINGLE_COIL:
        if (plen != 5 || (v != 0x0000u && v != 0xFF00u)) {
            return GW_MBAP_EX_ILLEGAL_VALUE;
        }
        rq->value = v == 0xFF00u ? 1u : 0u;
        return 0;
    case MB_FC_WRITE_SINGLE_REGISTER:
        if (plen != 5) {
            return GW_MBAP_EX_ILLEGAL_VALUE;
        }
        rq->value = v;
        return 0;
    case MB_FC_WRITE_MULTIPLE_COILS: {
        size_t nbytes = (v + 7u) / 8u;
        if (v == 0 || v > MB_MAX_WRITE_BITS || plen != 6 + nbytes || pdu[5] != nbytes ||
            (uint32_t)rq->addr + v > 0x10000u) {
            return GW_MBAP_EX_ILLEGAL_VALUE;
        }
        memcpy(txn->bits, &pdu[6], nbytes);
        rq->qty = v;
        rq->bits = txn->bits;
        return 0;
    }
    default: {   /* FC16 */
        size_t nbytes = 2u * v;
        if (v == 0 || v > MB_MAX_WRITE_REGS || plen != 6 + nbytes || pdu[5] != nbytes ||
            (uint32_t)rq->addr + v > 0x10000u) {
            return GW_MBAP_EX_ILLEGAL_VALUE;
        }
        for (uint16_t i = 0; i < v; i++) {
            txn->regs[i] = get16(&pdu[6 + 2u * i]);
        }
        rq->qty = v;
        rq->regs = txn->regs;
        return 0;
    }
    }
}

void gw_mbap_execute(mb_master_t *mb, gw_mbap_txn_t *txn)
{
    mb_response_t rsp;
    txn->status = mb_master_execute(mb, &txn->rq, &rsp);
    txn->exception = txn->status == MB_ERR_EXCEPTION ? rsp.exception : 0;
    txn->byte_count = 0;
    if (txn->status == MB_OK && txn->rq.fc <= MB_FC_READ_INPUT_REGISTERS) {
        txn->byte_count = rsp.byte_count;
        memcpy(txn->data, rsp.data, rsp.byte_count);   /* rsp.data is the master's rx buffer */
    }
}

static size_t header(uint8_t *out, uint16_t tid, uint8_t unit, size_t pdu_len)
{
    put16(&out[0], tid);
    put16(&out[2], 0);
    put16(&out[4], (uint16_t)(pdu_len + 1u));
    out[6] = unit;
    return GW_MBAP_HDR_LEN;
}

size_t gw_mbap_exception(const uint8_t *frame, uint8_t code, uint8_t *out, size_t cap)
{
    if (cap < GW_MBAP_HDR_LEN + 2) {
        return 0;
    }
    size_t n = header(out, get16(frame), frame[6], 2);
    out[n++] = (uint8_t)(frame[GW_MBAP_HDR_LEN] | 0x80u);
    out[n++] = code;
    return n;
}

size_t gw_mbap_response(const gw_mbap_txn_t *txn, uint8_t *out, size_t cap)
{
    const mb_request_t *rq = &txn->rq;
    uint8_t code = 0;
    if (txn->status == MB_ERR_EXCEPTION) {
        code = txn->exception;
    } else if (txn->status == MB_ERR_ARG) {
        code = GW_MBAP_EX_ILLEGAL_VALUE;
    } else if (txn->status != MB_OK) {
        code = GW_MBAP_EX_TARGET_NO_ANSWER;   /* timeout, CRC or framing errors after retries */
    }
    if (code != 0) {
        if (cap < GW_MBAP_HDR_LEN + 2) {
            return 0;
        }
        size_t n = header(out, txn->tid, rq->unit, 2);
        out[n++] = (uint8_t)(rq->fc | 0x80u);
        out[n++] = code;
        return n;
    }
    if (rq->fc <= MB_FC_READ_INPUT_REGISTERS) {
        size_t pdu = 2u + txn->byte_count;
        if (cap < GW_MBAP_HDR_LEN + pdu) {
            return 0;
        }
        size_t n = header(out, txn->tid, rq->unit, pdu);
        out[n++] = (uint8_t)rq->fc;
        out[n++] = txn->byte_count;
        memcpy(&out[n], txn->data, txn->byte_count);
        return n + txn->byte_count;
    }
    if (cap < GW_MBAP_HDR_LEN + 5) {
        return 0;
    }
    size_t n = header(out, txn->tid, rq->unit, 5);
    out[n++] = (uint8_t)rq->fc;
    put16(&out[n], rq->addr);
    uint16_t v = rq->fc == MB_FC_WRITE_SINGLE_COIL       ? (rq->value ? 0xFF00u : 0x0000u)
                 : rq->fc == MB_FC_WRITE_SINGLE_REGISTER ? rq->value
                                                         : rq->qty;
    put16(&out[n + 2], v);
    return n + 4;
}

bool gw_mbap_client_allowed(const gw_config_t *cfg, const uint8_t ip[4])
{
    if (cfg->modbus_tcp.client_count == 0) {
        return true;
    }
    uint32_t a = (uint32_t)ip[0] << 24 | (uint32_t)ip[1] << 16 | (uint32_t)ip[2] << 8 | ip[3];
    for (uint8_t i = 0; i < cfg->modbus_tcp.client_count; i++) {
        const uint8_t *n = cfg->modbus_tcp.clients[i].ip;
        uint8_t prefix = cfg->modbus_tcp.clients[i].prefix;
        uint32_t b = (uint32_t)n[0] << 24 | (uint32_t)n[1] << 16 | (uint32_t)n[2] << 8 | n[3];
        uint32_t mask = prefix == 0 ? 0u : 0xFFFFFFFFu << (32u - prefix);
        if ((a & mask) == (b & mask)) {
            return true;
        }
    }
    return false;
}

bool gw_mbap_via_ap(const uint8_t local[4], const uint8_t peer[4], const uint8_t *ap_ip,
                    const uint8_t *ap_mask)
{
    static const uint8_t none[4] = {0, 0, 0, 0};
    if (ap_ip == NULL || memcmp(ap_ip, none, 4) == 0) {
        return false;
    }
    if (memcmp(local, ap_ip, 4) == 0) {
        return true;
    }
    if (ap_mask == NULL || memcmp(ap_mask, none, 4) == 0) {
        return false;   /* no subnet: only the local address counts */
    }
    for (int i = 0; i < 4; i++) {
        if ((peer[i] & ap_mask[i]) != (ap_ip[i] & ap_mask[i])) {
            return false;
        }
    }
    return true;
}

/* ---------- counters (#69) ---------- */

static gw_mbap_stats_t s_stats;

static void add_mb(mb_stats_t *a, const mb_stats_t *b)
{
    a->ok += b->ok;
    a->timeout += b->timeout;
    a->crc += b->crc;
    a->frame += b->frame;
    a->exception += b->exception;
}

void gw_mbap_stats_add(const gw_mbap_stats_t *d)
{
    gw_port_lock();
    s_stats.accepted += d->accepted;
    s_stats.rejected += d->rejected;
    s_stats.forwarded += d->forwarded;
    s_stats.ex_function += d->ex_function;
    s_stats.ex_value += d->ex_value;
    s_stats.ex_path += d->ex_path;
    s_stats.ex_no_answer += d->ex_no_answer;
    add_mb(&s_stats.other_units, &d->other_units);
    s_stats.local += d->local;
    gw_port_unlock();
}

void gw_mbap_stats_get(gw_mbap_stats_t *out)
{
    gw_port_lock();
    *out = s_stats;
    gw_port_unlock();
}

void gw_mbap_stats_reset(void)
{
    gw_port_lock();
    memset(&s_stats, 0, sizeof(s_stats));
    gw_port_unlock();
}

void gw_mbap_count_exception(uint8_t code)
{
    gw_mbap_stats_t d = {0};
    d.ex_function = code == GW_MBAP_EX_ILLEGAL_FUNCTION;
    d.ex_value = code == GW_MBAP_EX_ILLEGAL_VALUE;
    d.ex_path = code == GW_MBAP_EX_PATH_UNAVAILABLE;
    d.ex_no_answer = code == GW_MBAP_EX_TARGET_NO_ANSWER;
    gw_mbap_stats_add(&d);
}
