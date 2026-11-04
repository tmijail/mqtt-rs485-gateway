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

/** Free entries in the write queue (called by the only producer, the Sparkplug task). */
size_t gw_bus_write_room(void);

#ifdef __cplusplus
}
#endif
