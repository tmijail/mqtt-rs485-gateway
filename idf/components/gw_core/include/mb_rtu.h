/*
 * Modbus RTU framing (pure functions): CRC-16, request ADU construction and
 * response ADU validation for FC01-FC06, FC15 and FC16.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_ADU_MAX 256

#define MB_MAX_READ_REGS   125
#define MB_MAX_READ_BITS   2000
#define MB_MAX_WRITE_REGS  123
#define MB_MAX_WRITE_BITS  1968

typedef enum {
    MB_FC_READ_COILS = 0x01,
    MB_FC_READ_DISCRETE_INPUTS = 0x02,
    MB_FC_READ_HOLDING_REGISTERS = 0x03,
    MB_FC_READ_INPUT_REGISTERS = 0x04,
    MB_FC_WRITE_SINGLE_COIL = 0x05,
    MB_FC_WRITE_SINGLE_REGISTER = 0x06,
    MB_FC_WRITE_MULTIPLE_COILS = 0x0F,
    MB_FC_WRITE_MULTIPLE_REGISTERS = 0x10,
} mb_fc_t;

typedef enum {
    MB_OK = 0,
    MB_ERR_TIMEOUT,     /* no byte received within the response timeout */
    MB_ERR_CRC,         /* complete frame with a bad CRC */
    MB_ERR_FRAME,       /* wrong unit/function, bad length, short frame, bad echo */
    MB_ERR_EXCEPTION,   /* valid exception response; see mb_response_t.exception */
    MB_ERR_ARG,         /* request cannot be encoded */
} mb_status_t;

typedef struct {
    uint8_t unit;
    mb_fc_t fc;
    uint16_t addr;
    uint16_t qty;                /* reads, FC15, FC16; ignored for FC05/FC06 */
    uint16_t value;              /* FC05 (0/1) and FC06 */
    const uint16_t *regs;        /* FC16: qty registers */
    const uint8_t *bits;         /* FC15: qty bits, LSB first, packed */
} mb_request_t;

typedef struct {
    uint8_t exception;           /* exception code when status == MB_ERR_EXCEPTION */
    const uint8_t *data;         /* FC01-04: payload bytes (points into the rx buffer) */
    uint8_t byte_count;
} mb_response_t;

uint16_t mb_crc16(const uint8_t *data, size_t len);

/** Build the request ADU (address + PDU + CRC). Returns its length, or 0 if invalid. */
size_t mb_build_request(const mb_request_t *rq, uint8_t *adu, size_t cap);

/**
 * Total response length implied by the first bytes received (at least 2; 3 for
 * FC01-04 normal responses). Returns 0 if more header bytes are needed and
 * SIZE_MAX if the header cannot belong to a response to @p rq.
 */
size_t mb_response_length(const mb_request_t *rq, const uint8_t *adu, size_t have);

/** Validate a complete response ADU against its request. */
mb_status_t mb_parse_response(const mb_request_t *rq, const uint8_t *adu, size_t len,
                              mb_response_t *rsp);

/** Register @p i (big-endian on the wire) of an FC03/FC04 response. */
uint16_t mb_rsp_reg(const mb_response_t *rsp, uint16_t i);

/** Bit @p i of an FC01/FC02 response. */
bool mb_rsp_bit(const mb_response_t *rsp, uint16_t i);

#ifdef __cplusplus
}
#endif
