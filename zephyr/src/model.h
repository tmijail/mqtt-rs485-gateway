/*
 * Shared runtime state of the gateway.
 *
 * Writers:  the RS485 bus owner (tag values, device online state) and the
 *           local I/O module (DI/DO state).
 * Reader:   the Sparkplug edge node, which turns state changes into
 *           BIRTH/DATA/DEATH messages.
 *
 * State is coalesced rather than queued: a slow or absent broker never makes
 * memory grow, it only delays the next DATA message, which then carries the
 * latest values (MQTT-7, REL-2). All storage is static.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_MODEL_H_
#define GW_MODEL_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include "gw_config.h"

#define GW_DI_COUNT 8
#define GW_DO_COUNT 8

BUILD_ASSERT(CONFIG_GW_MAX_DEVICES <= 32, "device dirty mask is 32 bits wide");

struct gw_tag_state {
	double value;    /* engineering value of the last good read */
	double reported; /* value last published (BIRTH or DATA) */
	int64_t ts_ms;   /* acquisition time of @c value */
	bool valid;      /* at least one good read since boot (no quality tracking, SPB-16) */
	bool pending;    /* changed beyond the deadband since @c reported */
};

struct gw_dev_state {
	bool online;
	uint32_t generation; /* incremented on every offline -> online transition */
	uint16_t fail_cycles;
	uint32_t ok_count;
	uint32_t timeout_count;
	uint32_t error_count; /* CRC/framing and Modbus exception responses */
};

struct gw_io_state {
	uint8_t di;          /* debounced inputs, bit n = DI/(n+1) */
	uint8_t outputs;     /* acknowledged output state, bit n = DO/(n+1) */
	int64_t di_ts_ms;
	int64_t do_ts_ms;
};

enum gw_status_flag {
	GW_STATUS_NET_UP = BIT(0),
	GW_STATUS_MQTT_UP = BIT(1),
	GW_STATUS_CONFIG_ERROR = BIT(2),
	GW_STATUS_MODBUS_ERROR = BIT(3), /* at least one device offline */
};

/** Bind the model to the (immutable) running configuration. */
void model_init(const struct gw_config *cfg);

const struct gw_config *model_cfg(void);

/* The model lock protects every gw_*_state returned below. */
void model_lock(void);
void model_unlock(void);

struct gw_tag_state *model_tag(uint16_t tag);
struct gw_dev_state *model_dev(uint8_t dev);
struct gw_io_state *model_io(void);

/**
 * Store a fresh tag value (caller holds the lock). The tag becomes pending
 * if it moved beyond its deadband since the last report, or unconditionally
 * when @p force is set (write confirmation).
 */
void model_tag_store(uint16_t tag, double value, int64_t ts_ms, bool force);

/**
 * Report-by-exception decision (SPB-7, SPB-12): true if @p now must be
 * published given the last reported value. Bool and integer tags without a
 * deadband report on any change; analog tags only when the absolute change
 * exceeds the deadband. A NaN appearing or disappearing is a change.
 */
bool model_rbe_exceeds(const struct gw_tag_cfg *t, double reported, double now);

/* ---- change notification towards the edge node ------------------------ */

/** Callback run (from the producer's context) when something became dirty. */
void model_set_notify(void (*notify)(void));

void model_signal_device(uint8_t dev);
void model_signal_node(void);

/** Atomically fetch and clear the set of devices with pending changes. */
uint32_t model_take_dirty_devices(void);

/** Atomically fetch and clear the node (local I/O) change flag. */
bool model_take_node_dirty(void);

/* ---- coarse status for the LED and the shell ------------------------- */

void model_status_set(enum gw_status_flag flag, bool on);
uint32_t model_status(void);

#endif /* GW_MODEL_H_ */
