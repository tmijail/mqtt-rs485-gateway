/*
 * Issue #182: the Modbus test read of the web UI and the REST API (POST /api/modbus/read), for
 * commissioning: is a slave there, and what does it answer, before the tags are configured.
 * This module parses the request and writes the answer; gw_web runs the read through the bus
 * owner (gw_bus_diag_read, MB-16), as the shell command mbread (CFG-6) does.
 *
 * Request (JSON object, no other keys):
 *   "unit"        1-247, required
 *   "area"        "coil" | "discrete" | "holding" | "input", required
 *   "address"     0-65535, required
 *   "count"       1-GW_MBDIAG_MAX registers or bits; default 1, or the width of "type"
 *   "type"        optional, a tag type of the configuration: the answer adds the converted
 *                 value of the first registers. "bool" only for coil/discrete (the first bit);
 *                 string and bytes take "count" registers
 *   "word_order"  optional, with "type" only: big|little|ABCD|CDAB|BADC|DCBA (default big)
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"
#include "mb_rtu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MBDIAG_MAX 16   /* registers or bits of one test read (GW_BUS_DIAG_MAX) */

typedef struct {
    uint8_t unit;
    gw_area_t area;
    uint16_t address;
    uint16_t count;
    bool convert;          /* "type" given: tag holds the conversion */
    gw_tag_cfg_t tag;      /* type, word_order, length; scale 1, offset 0 */
} gw_mbdiag_req_t;

/** Parse the request body. False with the reason (English) in @p why. Takes the JSON lock. */
bool gw_mbdiag_parse(const char *json, size_t len, gw_mbdiag_req_t *rq, char *why, size_t cap);

/** The read function of the request's area (FC01-FC04). */
mb_fc_t gw_mbdiag_fc(const gw_mbdiag_req_t *rq);

/**
 * The answer to the read: the JSON body in @p out (at most @p cap bytes, NUL terminated) and the
 * HTTP status as the return value. MB_OK: "200 OK", {"ok":true, the request, "values":[...] and,
 * with a type, "value"}. The Modbus errors: "504 Gateway Timeout" (no answer) or "502 Bad
 * Gateway" (CRC, framing, an exception with its code), with "error" and "result". MB_ERR_ARG
 * (the bus did not run the read in the wait, or another test read is pending): "503 Service
 * Unavailable". @p values has rq->count registers or bits (MB_OK only).
 */
const char *gw_mbdiag_answer(const gw_mbdiag_req_t *rq, mb_status_t st, uint8_t exception,
                             const uint16_t *values, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
