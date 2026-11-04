/*
 * RS485 bus owner (MB-16): the only task that talks to the Modbus master. It
 * runs the poll schedule and executes DCMD writes, which are queued and served
 * before the next poll (MB-14 at transaction granularity).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"
#include "mb_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*device_updated)(uint16_t dev);       /* after every poll cycle of @p dev */
    void (*write_done)(uint16_t tag, bool ok);  /* after every queued write */
} gw_bus_callbacks_t;

esp_err_t gw_bus_start(const gw_config_t *cfg, const mb_transport_t *tr, const gw_bus_callbacks_t *cb);

/** Queue a write of engineering @p value to @p tag. Non-blocking; false if the queue is full. */
bool gw_bus_request_write(uint16_t tag, double value);

/**
 * Before a restart: let the transaction in progress finish and start no other, so the
 * restart never cuts a frame on the wire (a write half sent) and leaves the UART idle.
 * Waits at most @p timeout_ms; the bus stays quiet until the restart.
 */
void gw_bus_quiesce(uint32_t timeout_ms);

#define GW_BUS_DIAG_MAX 16   /* registers or bits per diagnostic read */

/**
 * CFG-6 diagnostics: one Modbus read (FC01-FC04) executed by the bus task between two poll
 * transactions, so the bus keeps a single owner (MB-16). Blocks the caller (never the bus)
 * until the answer, at most @p timeout_ms. @p out receives @p count registers or bits (0/1).
 * Returns MB_ERR_ARG if the bus is not running, another diagnostic read is pending, the
 * queue is full or the wait timed out; MB_ERR_EXCEPTION with the code in @p exception.
 */
mb_status_t gw_bus_diag_read(uint8_t unit, mb_fc_t fc, uint16_t addr, uint16_t count,
                             uint16_t out[GW_BUS_DIAG_MAX], uint8_t *exception,
                             uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
