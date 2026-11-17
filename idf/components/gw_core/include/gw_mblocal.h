/*
 * #216: the local unit of the Modbus TCP server. The gateway answers modbus_tcp.local_unit_id
 * itself, with its own I/O, and never sends that unit to the RS485 bus:
 *
 *   discrete inputs (FC02) 0..7   DI/1..DI/8
 *   coils           (FC01) 0..7   DO/1..DO/8 (FC05 and FC15 with modbus_tcp.local_writes)
 *   input registers (FC04) 0..31  the pulse counters of DI/1..DI/8, 4 registers each (uint64,
 *                                 the most significant register first: "big")
 *
 * Pure functions on a snapshot of the I/O; the server task (gw_mbtcp) takes the snapshot from
 * gw_tagdb and sends the DO writes to the local_io task, the only writer of the DO expander.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_config.h"
#include "gw_mbap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_MBLOCAL_EX_ILLEGAL_ADDRESS 0x02   /* outside the map above */
#define GW_MBLOCAL_EX_DEVICE_FAILURE  0x04   /* the DO expander does not answer */
#define GW_MBLOCAL_REGS_PER_COUNTER   4

/** The I/O of the gateway at the time of the request (logical states: 1 = active / on). */
typedef struct {
    uint8_t di;                 /* bit n = DI/n+1 */
    uint8_t dout;               /* bit n = DO/n+1 */
    bool do_known;              /* false: the expander does not answer, the DOs are unknown */
    uint8_t rule_do;            /* bit n: a local rule drives DO/n+1 (IO-7) */
    uint64_t counters[GW_LOCAL_IO_CHANNELS];
} gw_mblocal_io_t;

/** The DOs that a write request changes: for each bit of @p mask, the state in @p value. */
typedef struct {
    uint8_t mask;
    uint8_t value;
} gw_mblocal_write_t;

/** True when @p unit is the local unit of @p cfg (never for 0: the local unit is off). */
bool gw_mblocal_is_local(const gw_config_t *cfg, uint8_t unit);

/**
 * Answer the parsed request @p txn (gw_mbap_parse() returned 0) from @p io. Sets txn->status to
 * MB_OK (the data of a read is in txn->data) or to MB_ERR_EXCEPTION with txn->exception:
 *   01 for FC03, FC06 and FC16 (no holding registers), and for FC05 and FC15 without
 *      local_writes;
 *   02 for an address outside the map;
 *   03 for a write to a DO that a local rule drives (the same refusal as an NCMD);
 *   04 for a read or a write of the DOs while the expander does not answer.
 * For an accepted write, @p wr gets the DOs to change (else its mask is 0); the caller applies
 * them, then answers with gw_mbap_response(), or calls gw_mblocal_write_failed() first.
 */
void gw_mblocal_execute(const gw_config_t *cfg, const gw_mblocal_io_t *io, gw_mbap_txn_t *txn,
                        gw_mblocal_write_t *wr);

/** The local_io task did not apply the write of @p txn: exception 04. */
void gw_mblocal_write_failed(gw_mbap_txn_t *txn);

#ifdef __cplusplus
}
#endif
