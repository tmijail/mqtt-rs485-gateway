#include "gw_mblocal.h"

#include <string.h>

bool gw_mblocal_is_local(const gw_config_t *cfg, uint8_t unit)
{
    return cfg->modbus_tcp.local_unit_id != 0 && unit == cfg->modbus_tcp.local_unit_id;
}

static void refuse(gw_mbap_txn_t *txn, uint8_t code)
{
    txn->status = MB_ERR_EXCEPTION;
    txn->exception = code;
    txn->byte_count = 0;
}

/* @p qty bits of @p bits from @p addr, packed as Modbus wants them: the first bit in the least
 * significant bit of the first byte. */
static void pack_bits(gw_mbap_txn_t *txn, uint8_t bits, uint16_t addr, uint16_t qty)
{
    txn->byte_count = (uint8_t)((qty + 7u) / 8u);
    memset(txn->data, 0, txn->byte_count);
    for (uint16_t i = 0; i < qty; i++) {
        if ((bits >> (addr + i)) & 1u) {
            txn->data[i / 8u] |= (uint8_t)(1u << (i % 8u));
        }
    }
}

/* The uint64 of input registers 4 * @p k .. 4 * @p k + 3: the counters of DI/1..8 (k 0..7), then
 * the run hours of DI/1..8 and DO/1..8 (#234; k 8..23). A channel that is not in
 * local_io.run_hours has no Run Hours metric: it reads 0. */
static uint64_t input_value(const gw_config_t *cfg, const gw_mblocal_io_t *io, unsigned k)
{
    if (k < GW_LOCAL_IO_CHANNELS) {
        return io->counters[k];
    }
    k -= GW_LOCAL_IO_CHANNELS;
    return (cfg->local_io.run_hours & (1u << k)) ? io->run_s[k] : 0u;
}

void gw_mblocal_execute(const gw_config_t *cfg, const gw_mblocal_io_t *io, gw_mbap_txn_t *txn,
                        gw_mblocal_write_t *wr)
{
    const mb_request_t *rq = &txn->rq;
    wr->mask = wr->value = 0;
    txn->status = MB_OK;
    txn->exception = 0;
    txn->byte_count = 0;
    /* the end of the addressed range; FC05 and FC06 address one item */
    uint32_t qty = rq->fc == MB_FC_WRITE_SINGLE_COIL || rq->fc == MB_FC_WRITE_SINGLE_REGISTER
                       ? 1u
                       : rq->qty;
    uint32_t end = (uint32_t)rq->addr + qty;
    switch (rq->fc) {
    case MB_FC_READ_DISCRETE_INPUTS:
        if (end > GW_LOCAL_IO_CHANNELS) {
            refuse(txn, GW_MBLOCAL_EX_ILLEGAL_ADDRESS);
            return;
        }
        pack_bits(txn, io->di, rq->addr, rq->qty);
        return;
    case MB_FC_READ_COILS:
        if (end > GW_LOCAL_IO_CHANNELS) {
            refuse(txn, GW_MBLOCAL_EX_ILLEGAL_ADDRESS);
        } else if (!io->do_known) {
            refuse(txn, GW_MBLOCAL_EX_DEVICE_FAILURE);
        } else {
            pack_bits(txn, io->dout, rq->addr, rq->qty);
        }
        return;
    case MB_FC_READ_INPUT_REGISTERS:
        if (end > GW_MBLOCAL_INPUT_REGS) {   /* also a range that crosses the end */
            refuse(txn, GW_MBLOCAL_EX_ILLEGAL_ADDRESS);
            return;
        }
        for (uint16_t i = 0; i < rq->qty; i++) {
            unsigned r = rq->addr + i;
            unsigned shift = 16u * (GW_MBLOCAL_REGS_PER_COUNTER - 1u - r % GW_MBLOCAL_REGS_PER_COUNTER);
            uint16_t v = (uint16_t)(input_value(cfg, io, r / GW_MBLOCAL_REGS_PER_COUNTER) >> shift);
            txn->data[2u * i] = (uint8_t)(v >> 8);
            txn->data[2u * i + 1u] = (uint8_t)v;
        }
        txn->byte_count = (uint8_t)(2u * rq->qty);
        return;
    case MB_FC_WRITE_SINGLE_COIL:
    case MB_FC_WRITE_MULTIPLE_COILS: {
        if (!cfg->modbus_tcp.local_writes) {
            refuse(txn, GW_MBAP_EX_ILLEGAL_FUNCTION);
            return;
        }
        if (end > GW_LOCAL_IO_CHANNELS) {
            refuse(txn, GW_MBLOCAL_EX_ILLEGAL_ADDRESS);
            return;
        }
        uint8_t mask = 0, value = 0;
        for (uint32_t i = 0; i < qty; i++) {
            bool on = rq->fc == MB_FC_WRITE_SINGLE_COIL ? rq->value != 0
                                                        : ((rq->bits[i / 8u] >> (i % 8u)) & 1u) != 0;
            mask |= (uint8_t)(1u << (rq->addr + i));
            value |= (uint8_t)((on ? 1u : 0u) << (rq->addr + i));
        }
        if (mask & io->rule_do) {
            refuse(txn, GW_MBAP_EX_ILLEGAL_VALUE);   /* IO-7: the rule owns that DO */
        } else if (!io->do_known) {
            refuse(txn, GW_MBLOCAL_EX_DEVICE_FAILURE);
        } else {
            wr->mask = mask;
            wr->value = value;
        }
        return;
    }
    default:   /* FC03, FC06, FC16: the local unit has no holding registers */
        refuse(txn, GW_MBAP_EX_ILLEGAL_FUNCTION);
        return;
    }
}

void gw_mblocal_write_failed(gw_mbap_txn_t *txn)
{
    refuse(txn, GW_MBLOCAL_EX_DEVICE_FAILURE);
}
