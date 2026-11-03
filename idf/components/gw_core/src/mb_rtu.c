#include "mb_rtu.h"

#include <stdint.h>
#include <string.h>

uint16_t mb_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static size_t finish(uint8_t *adu, size_t len)
{
    uint16_t crc = mb_crc16(adu, len);
    adu[len] = (uint8_t)crc;          /* CRC is transmitted low byte first */
    adu[len + 1] = (uint8_t)(crc >> 8);
    return len + 2;
}

static bool is_read(mb_fc_t fc)
{
    return fc >= MB_FC_READ_COILS && fc <= MB_FC_READ_INPUT_REGISTERS;
}

size_t mb_build_request(const mb_request_t *rq, uint8_t *adu, size_t cap)
{
    if (rq->unit == 0 || rq->unit > 247 || cap < 8) {
        return 0;
    }
    adu[0] = rq->unit;
    adu[1] = (uint8_t)rq->fc;
    put16(&adu[2], rq->addr);

    switch (rq->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
    case MB_FC_READ_HOLDING_REGISTERS:
    case MB_FC_READ_INPUT_REGISTERS: {
        uint16_t max = rq->fc <= MB_FC_READ_DISCRETE_INPUTS ? MB_MAX_READ_BITS : MB_MAX_READ_REGS;
        if (rq->qty == 0 || rq->qty > max || (uint32_t)rq->addr + rq->qty > 0x10000u) {
            return 0;
        }
        put16(&adu[4], rq->qty);
        return finish(adu, 6);
    }
    case MB_FC_WRITE_SINGLE_COIL:
        put16(&adu[4], rq->value ? 0xFF00u : 0x0000u);
        return finish(adu, 6);
    case MB_FC_WRITE_SINGLE_REGISTER:
        put16(&adu[4], rq->value);
        return finish(adu, 6);
    case MB_FC_WRITE_MULTIPLE_COILS: {
        if (rq->bits == NULL || rq->qty == 0 || rq->qty > MB_MAX_WRITE_BITS ||
            (uint32_t)rq->addr + rq->qty > 0x10000u) {
            return 0;
        }
        size_t nbytes = (rq->qty + 7u) / 8u;
        if (cap < 7 + nbytes + 2) {
            return 0;
        }
        put16(&adu[4], rq->qty);
        adu[6] = (uint8_t)nbytes;
        memcpy(&adu[7], rq->bits, nbytes);
        if (rq->qty % 8u) {
            adu[7 + nbytes - 1] &= (uint8_t)((1u << (rq->qty % 8u)) - 1u);
        }
        return finish(adu, 7 + nbytes);
    }
    case MB_FC_WRITE_MULTIPLE_REGISTERS: {
        if (rq->regs == NULL || rq->qty == 0 || rq->qty > MB_MAX_WRITE_REGS ||
            (uint32_t)rq->addr + rq->qty > 0x10000u) {
            return 0;
        }
        size_t nbytes = (size_t)rq->qty * 2u;
        if (cap < 7 + nbytes + 2) {
            return 0;
        }
        put16(&adu[4], rq->qty);
        adu[6] = (uint8_t)nbytes;
        for (uint16_t i = 0; i < rq->qty; i++) {
            put16(&adu[7 + 2u * i], rq->regs[i]);
        }
        return finish(adu, 7 + nbytes);
    }
    default:
        return 0;
    }
}

size_t mb_response_length(const mb_request_t *rq, const uint8_t *adu, size_t have)
{
    if (have < 2) {
        return 0;
    }
    if (adu[0] != rq->unit) {
        return SIZE_MAX;
    }
    if (adu[1] == ((uint8_t)rq->fc | 0x80u)) {
        return 5;  /* unit, fc|0x80, exception code, crc16 */
    }
    if (adu[1] != (uint8_t)rq->fc) {
        return SIZE_MAX;
    }
    if (!is_read(rq->fc)) {
        return 8;  /* FC05/06/15/16 echo: unit, fc, addr, value/qty, crc16 */
    }
    if (have < 3) {
        return 0;
    }
    return 3u + adu[2] + 2u;
}

mb_status_t mb_parse_response(const mb_request_t *rq, const uint8_t *adu, size_t len,
                              mb_response_t *rsp)
{
    memset(rsp, 0, sizeof(*rsp));
    size_t expect = mb_response_length(rq, adu, len);
    if (expect == 0 || expect == SIZE_MAX || len != expect) {
        return MB_ERR_FRAME;
    }
    uint16_t crc = (uint16_t)(adu[len - 2] | (adu[len - 1] << 8));
    if (crc != mb_crc16(adu, len - 2)) {
        return MB_ERR_CRC;
    }
    if (adu[1] & 0x80u) {
        rsp->exception = adu[2];
        return MB_ERR_EXCEPTION;
    }
    switch (rq->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE_INPUTS:
        if (adu[2] != (rq->qty + 7u) / 8u) {
            return MB_ERR_FRAME;
        }
        break;
    case MB_FC_READ_HOLDING_REGISTERS:
    case MB_FC_READ_INPUT_REGISTERS:
        if (adu[2] != rq->qty * 2u) {
            return MB_ERR_FRAME;
        }
        break;
    case MB_FC_WRITE_SINGLE_COIL:
        if (get16(&adu[2]) != rq->addr || get16(&adu[4]) != (rq->value ? 0xFF00u : 0x0000u)) {
            return MB_ERR_FRAME;
        }
        return MB_OK;
    case MB_FC_WRITE_SINGLE_REGISTER:
        if (get16(&adu[2]) != rq->addr || get16(&adu[4]) != rq->value) {
            return MB_ERR_FRAME;
        }
        return MB_OK;
    case MB_FC_WRITE_MULTIPLE_COILS:
    case MB_FC_WRITE_MULTIPLE_REGISTERS:
        if (get16(&adu[2]) != rq->addr || get16(&adu[4]) != rq->qty) {
            return MB_ERR_FRAME;
        }
        return MB_OK;
    default:
        return MB_ERR_FRAME;
    }
    rsp->byte_count = adu[2];
    rsp->data = &adu[3];
    return MB_OK;
}

uint16_t mb_rsp_reg(const mb_response_t *rsp, uint16_t i)
{
    return get16(&rsp->data[2u * i]);
}

bool mb_rsp_bit(const mb_response_t *rsp, uint16_t i)
{
    return (rsp->data[i / 8u] >> (i % 8u)) & 1u;
}
