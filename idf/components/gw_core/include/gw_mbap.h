/*
 * Modbus TCP gateway framing (MB-15): MBAP frames from TCP clients are turned into RTU
 * requests for the bus owner, and the RTU outcome back into an MBAP response. Pure functions;
 * the socket server lives in gw_mbtcp and the transaction runs in the bus owner task (MB-16).
 *
 * Forwarded: FC01-FC06, FC15 and FC16 to units 1..247. Anything else is answered by the
 * gateway itself with an exception (01 illegal function, 03 illegal data value, 0A gateway
 * path unavailable); a unit that does not answer gives 0B (gateway target failed to respond).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"
#include "mb_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MBAP_HDR_LEN  7     /* transaction id, protocol id, length, unit id */
#define GW_MBAP_ADU_MAX  260   /* 7 + 253-byte PDU */

#define GW_MBAP_EX_ILLEGAL_FUNCTION  0x01
#define GW_MBAP_EX_ILLEGAL_VALUE     0x03
#define GW_MBAP_EX_PATH_UNAVAILABLE  0x0A
#define GW_MBAP_EX_TARGET_NO_ANSWER  0x0B

/** One forwarded transaction: the request and, once the bus owner ran it, its outcome. */
typedef struct {
    uint16_t tid;                       /* MBAP transaction id, echoed */
    mb_request_t rq;                    /* .regs/.bits point into the arrays below */
    uint16_t regs[MB_MAX_WRITE_REGS];
    uint8_t bits[(MB_MAX_WRITE_BITS + 7) / 8];
    /* outcome, filled by gw_mbap_execute() */
    mb_status_t status;
    uint8_t exception;
    uint8_t byte_count;                 /* FC01-04 */
    uint8_t data[2 * MB_MAX_READ_REGS];
} gw_mbap_txn_t;

/**
 * Length of the frame that starts @p buf, from the @p have bytes received: 0 when the header
 * is not complete yet, SIZE_MAX when it cannot be MBAP (protocol id not 0, bad length): the
 * connection must then be closed, since the stream cannot be resynchronised.
 */
size_t gw_mbap_frame_len(const uint8_t *buf, size_t have);

/**
 * Parse a complete frame. Returns 0 when @p txn holds a request to forward, or the exception
 * code the gateway must answer itself (gw_mbap_exception()).
 */
uint8_t gw_mbap_parse(const uint8_t *frame, size_t len, gw_mbap_txn_t *txn);

/** Run @p txn on the bus (called by the bus owner task) and store the outcome. */
void gw_mbap_execute(mb_master_t *mb, gw_mbap_txn_t *txn);

/** MBAP exception response to @p frame (a complete, parseable header) with @p code. */
size_t gw_mbap_exception(const uint8_t *frame, uint8_t code, uint8_t *out, size_t cap);

/** MBAP response for an executed @p txn (normal or exception). Returns its length. */
size_t gw_mbap_response(const gw_mbap_txn_t *txn, uint8_t *out, size_t cap);

/** Whether a client at IPv4 @p ip may connect (empty list: any client). */
bool gw_mbap_client_allowed(const gw_config_t *cfg, const uint8_t ip[4]);

#ifdef __cplusplus
}
#endif
