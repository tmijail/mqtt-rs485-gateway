/*
 * RS485 bus owner (MB-16): the only task that talks to the Modbus master. It
 * runs the poll schedule and executes DCMD writes, which are queued and served
 * before the next poll and between the requests of a cycle (MB-14): a write waits
 * at most for the transaction in progress.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "gw_config.h"
#include "gw_mbap.h"
#include "gw_tagdb.h"
#include "mb_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*device_updated)(uint16_t dev);       /* after every poll cycle of @p dev */
    void (*write_done)(uint16_t tag, bool ok);  /* after every queued write */
} gw_bus_callbacks_t;

esp_err_t gw_bus_start(const gw_config_t *cfg, const mb_transport_t *tr, const gw_bus_callbacks_t *cb);

/** Queue a write of @p value (member per gw_conv_kind) to @p tag. Non-blocking; false if the
 * queue is full. */
bool gw_bus_request_write(uint16_t tag, const gw_tag_value_t *value);

/** SPB-15: poll every device every @p ms (0 = each at its poll_ms). Non-blocking. */
void gw_bus_set_scan_rate(uint32_t ms);

/**
 * MB-15: run a Modbus TCP transaction on the bus (between the requests of a cycle, after the
 * pending DCMD writes) and wait at most @p wait_ms for it. Single caller (the Modbus TCP
 * server). On false the transaction may still run later: @p txn must not be reused until
 * gw_bus_forward_idle() says so.
 */
bool gw_bus_forward(gw_mbap_txn_t *txn, uint32_t wait_ms);

/** true once no forwarded transaction is pending (waits at most @p wait_ms for it). */
bool gw_bus_forward_idle(uint32_t wait_ms);

/** Free entries in the write queue (called by the only producer, the Sparkplug task). */
size_t gw_bus_write_room(void);

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

#if CONFIG_GW_FW_PARK_OTHER_CORE
/**
 * Issue #111 (QEMU build only): before gw_fw parks core 1, the bus task, the only task of ours
 * pinned there, stops between two transactions or two attempts of one (issue #112), where it
 * holds no lock, and waits until gw_bus_release(). Waits at most @p timeout_ms for it, or the
 * longest attempt of the configured RS485 settings plus 1 s if that is longer; @p waited_ms
 * (may be NULL) receives the bound it used. True once it stopped (or the bus is not running),
 * false if it did not (then released again).
 *
 * Issue #112: while core 1 is parked, nothing may make a blocking IPC call to it
 * (esp_ipc_call(), esp_ipc_call_blocking()): with CONFIG_ESP_IPC_USES_CALLERS_PRIORITY ipc1 runs
 * at the caller's priority, below the park task, so the call waits until the park ends, and
 * deadlocks if the caller holds a lock the parked section needs. gw_fw wraps both calls at link
 * time in this build and logs "fw: blocking IPC to core 1 while it was parked" (an E2E failure).
 */
bool gw_bus_hold(uint32_t timeout_ms, uint32_t *waited_ms);
void gw_bus_release(void);
#endif

#if CONFIG_GW_IO_SIMULATED
/** Issue #111, emulator only: the bus task (core 1) floods the console for @p ms (0 stops it),
 * a burst of log lines between two transactions, so a park of core 1 finds it logging. */
void gw_bus_sim_log_storm(uint32_t ms);
#endif

#ifdef __cplusplus
}
#endif
