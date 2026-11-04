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

#ifdef __cplusplus
}
#endif
