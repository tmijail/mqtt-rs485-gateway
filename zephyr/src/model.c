/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "model.h"

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

static const struct gw_config *cfg;
static struct gw_tag_state tags[CONFIG_GW_MAX_TAGS];
static struct gw_dev_state devs[CONFIG_GW_MAX_DEVICES];
static struct gw_io_state io;

static K_MUTEX_DEFINE(lock);
static atomic_t dirty_devices;
static atomic_t node_dirty;
static atomic_t status;
static void (*notify_cb)(void);

void model_init(const struct gw_config *config)
{
	cfg = config;
	memset(tags, 0, sizeof(tags));
	memset(devs, 0, sizeof(devs));
	memset(&io, 0, sizeof(io));
	atomic_clear(&dirty_devices);
	atomic_clear(&node_dirty);
}

const struct gw_config *model_cfg(void)
{
	return cfg;
}

void model_lock(void)
{
	(void)k_mutex_lock(&lock, K_FOREVER);
}

void model_unlock(void)
{
	(void)k_mutex_unlock(&lock);
}

struct gw_tag_state *model_tag(uint16_t tag)
{
	return &tags[tag];
}

struct gw_dev_state *model_dev(uint8_t dev)
{
	return &devs[dev];
}

struct gw_io_state *model_io(void)
{
	return &io;
}

bool model_rbe_exceeds(const struct gw_tag_cfg *t, double reported, double now)
{
	if (isnan(now) || isnan(reported)) {
		return isnan(now) != isnan(reported);
	}
	if (t->type == GW_TYPE_BOOL || t->deadband <= 0.0) {
		return now != reported;
	}
	return fabs(now - reported) > t->deadband;
}

void model_tag_store(uint16_t tag, double value, int64_t ts_ms, bool force)
{
	struct gw_tag_state *st = &tags[tag];

	if (force || !st->valid || model_rbe_exceeds(&cfg->tags[tag], st->reported, value)) {
		st->pending = true;
	}
	st->value = value;
	st->ts_ms = ts_ms;
	st->valid = true;
}

void model_set_notify(void (*notify)(void))
{
	notify_cb = notify;
}

static void notify(void)
{
	void (*cb)(void) = notify_cb;

	if (cb != NULL) {
		cb();
	}
}

void model_signal_device(uint8_t dev)
{
	atomic_or(&dirty_devices, (atomic_val_t)BIT(dev));
	notify();
}

void model_signal_node(void)
{
	atomic_set(&node_dirty, 1);
	notify();
}

uint32_t model_take_dirty_devices(void)
{
	return (uint32_t)atomic_clear(&dirty_devices);
}

bool model_take_node_dirty(void)
{
	return atomic_clear(&node_dirty) != 0;
}

void model_status_set(enum gw_status_flag flag, bool on)
{
	if (on) {
		atomic_or(&status, (atomic_val_t)flag);
	} else {
		atomic_and(&status, ~(atomic_val_t)flag);
	}
}

uint32_t model_status(void)
{
	return (uint32_t)atomic_get(&status);
}
