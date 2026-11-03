/*
 * Modbus RTU master built on the Zephyr modbus subsystem (client role).
 *
 * The subsystem provides the RTU framing, CRC, the 3.5 character inter
 * frame timer and the RS485 driver-enable pin (de-gpios). This module adds
 * the gateway policy: retries, inter-transaction delay, offline detection,
 * conversion and the single-owner scheduling of reads and writes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mb_poller.h"

#include <errno.h>

#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/sys/util.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "convert.h"
#include "gw_time.h"
#include "model.h"

LOG_MODULE_REGISTER(mb_poller, CONFIG_GW_LOG_LEVEL);

#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

struct write_req {
	uint8_t dev;
	uint16_t tag;
	double value;
};

K_MSGQ_DEFINE(write_q, sizeof(struct write_req), CONFIG_GW_WRITE_QUEUE_LEN, 8);
K_THREAD_STACK_DEFINE(poller_stack, 3072);

static struct k_thread poller_thread;
static const struct gw_config *cfg;
static int iface = -1;
static int wdt_channel = -1;
static int64_t next_due[CONFIG_GW_MAX_DEVICES];

enum op {
	OP_READ,
	OP_WRITE,
};

static void feed_wdt(void)
{
	if (wdt_channel >= 0) {
		(void)task_wdt_feed(wdt_channel);
	}
}

static void inter_frame_delay(void)
{
	if (cfg->rs485.inter_frame_delay_ms > 0) {
		k_msleep(cfg->rs485.inter_frame_delay_ms);
	}
}

/* One Modbus request on the wire. regs[] holds the read result or the data
 * to write. Returns 0, a positive Modbus exception code or a negative errno.
 */
static int request(enum op op, uint8_t unit, const struct gw_tag_cfg *t, uint16_t regs[2])
{
	uint8_t nregs = gw_type_regs((enum gw_type)t->type);
	uint8_t bit = 0;
	int ret;

	switch (t->area) {
	case GW_AREA_COIL:
		if (op == OP_WRITE) {
			return modbus_write_coil(iface, unit, t->address, regs[0] != 0); /* FC05 */
		}
		ret = modbus_read_coils(iface, unit, t->address, &bit, 1); /* FC01 */
		regs[0] = bit & 1U;
		return ret;
	case GW_AREA_DISCRETE:
		ret = modbus_read_dinputs(iface, unit, t->address, &bit, 1); /* FC02 */
		regs[0] = bit & 1U;
		return ret;
	case GW_AREA_HOLDING:
		if (op == OP_WRITE) {
			return (nregs == 1) ? modbus_write_holding_reg(iface, unit, t->address,
								       regs[0]) /* FC06 */
					    : modbus_write_holding_regs(iface, unit, t->address,
									regs, nregs); /* FC16 */
		}
		return modbus_read_holding_regs(iface, unit, t->address, regs, nregs); /* FC03 */
	default:
		return modbus_read_input_regs(iface, unit, t->address, regs, nregs); /* FC04 */
	}
}

/* A request with the configured retries (MB-8). Exceptions are definitive
 * answers from the slave and are not retried.
 */
static int transact(enum op op, uint8_t dev, const struct gw_tag_cfg *t, uint16_t regs[2])
{
	const struct gw_device_cfg *d = &cfg->devices[dev];
	struct gw_dev_state *ds = model_dev(dev);
	int ret = -EIO;

	for (uint8_t attempt = 0; attempt <= cfg->rs485.retries; attempt++) {
		feed_wdt();
		ret = request(op, d->unit_id, t, regs);
		inter_frame_delay();

		model_lock();
		if (ret == 0) {
			ds->ok_count++;
		} else if (ret == -ETIMEDOUT) {
			ds->timeout_count++;
		} else {
			ds->error_count++;
		}
		model_unlock();

		if (ret >= 0) {
			break;
		}
	}
	if (ret > 0) {
		LOG_WRN("%s/%s: Modbus exception %d", d->name, t->name, ret);
	}
	return ret;
}

static void do_write(const struct write_req *w)
{
	const struct gw_device_cfg *d = &cfg->devices[w->dev];
	const struct gw_tag_cfg *t = &cfg->tags[w->tag];
	uint16_t regs[2] = {0};
	bool online;
	int ret;

	model_lock();
	online = model_dev(w->dev)->online;
	model_unlock();
	if (!online) {
		LOG_WRN("write %s/%s discarded: device offline", d->name, t->name);
		return;
	}
	ret = gw_value_to_regs(t, w->value, regs);
	if (ret < 0) {
		LOG_WRN("write %s/%s discarded: value out of range for its type", d->name, t->name);
		return;
	}
	ret = transact(OP_WRITE, w->dev, t, regs);
	if (ret != 0) {
		LOG_ERR("write %s/%s failed (%d)", d->name, t->name, ret);
		return;
	}

	/* Confirm with a read-back; fall back to the acknowledged value. */
	if (transact(OP_READ, w->dev, t, regs) != 0) {
		(void)gw_value_to_regs(t, w->value, regs);
	}
	model_lock();
	model_tag_store(w->tag, gw_regs_to_value(t, regs), gw_time_now_ms(), true);
	model_unlock();
	LOG_INF("write %s/%s confirmed", d->name, t->name);
	model_signal_device(w->dev);
}

static void serve_writes(void)
{
	struct write_req w;

	while (k_msgq_get(&write_q, &w, K_NO_WAIT) == 0) {
		do_write(&w);
	}
}

static void update_modbus_status(void)
{
	bool all_online = true;

	model_lock();
	for (uint8_t i = 0; i < cfg->device_count; i++) {
		all_online = all_online && model_dev(i)->online;
	}
	model_unlock();
	model_status_set(GW_STATUS_MODBUS_ERROR, !all_online);
}

/* One poll cycle of a device (MB-9, MB-10). A cycle fails as soon as a
 * request stays unanswered after its retries (a Modbus exception is an
 * answer): the rest of the cycle is skipped, so a device that disappears
 * is declared offline after about offline_after x (poll_ms + timeouts).
 * While the device is offline the first tag acts as the probe; once it
 * answers the whole device is read, so its DBIRTH carries current values.
 */
static void poll_device(uint8_t dev)
{
	const struct gw_device_cfg *d = &cfg->devices[dev];
	struct gw_dev_state *ds = model_dev(dev);
	bool was_online = ds->online; /* only this thread writes it */
	bool failed = false;

	for (uint16_t i = 0; i < d->tag_count && !failed; i++) {
		uint16_t tag = d->first_tag + i;
		uint16_t regs[2] = {0};
		int ret;

		serve_writes(); /* writes take priority over the poll (MB-14) */
		ret = transact(OP_READ, dev, &cfg->tags[tag], regs);
		if (ret == 0) {
			model_lock();
			model_tag_store(tag, gw_regs_to_value(&cfg->tags[tag], regs),
					gw_time_now_ms(), false);
			model_unlock();
		}
		failed = ret < 0;
	}

	model_lock();
	if (!failed) {
		ds->fail_cycles = 0;
		if (!was_online) {
			ds->online = true;
			ds->generation++;
		}
	} else if (ds->fail_cycles < UINT16_MAX) {
		ds->fail_cycles++;
		if (was_online && ds->fail_cycles >= d->offline_after) {
			ds->online = false;
		}
	}
	model_unlock();

	if (ds->online != was_online) {
		LOG_INF("device %s is %s", d->name, ds->online ? "online" : "offline");
		update_modbus_status();
	}
	if (ds->online || was_online) {
		model_signal_device(dev);
	}
}

static void poller_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	wdt_channel = task_wdt_add(CONFIG_GW_WDT_TIMEOUT_MS, NULL, NULL);
	update_modbus_status();

	for (uint8_t i = 0; i < cfg->device_count; i++) {
		next_due[i] = k_uptime_get();
	}

	while (true) {
		struct write_req w;
		int64_t now;
		int64_t wait;
		int next = -1;

		feed_wdt();
		serve_writes();

		now = k_uptime_get();
		for (uint8_t i = 0; i < cfg->device_count; i++) {
			if (next < 0 || next_due[i] < next_due[next]) {
				next = i;
			}
		}
		wait = (next < 0) ? 1000 : MIN(next_due[next] - now, 1000);
		if (wait > 0) {
			if (k_msgq_get(&write_q, &w, K_MSEC(wait)) == 0) {
				do_write(&w);
			}
			continue;
		}

		poll_device((uint8_t)next);
		next_due[next] += cfg->devices[next].poll_ms;
		if (next_due[next] <= k_uptime_get()) {
			/* Overrun: restart the period instead of bursting. */
			next_due[next] = k_uptime_get() + cfg->devices[next].poll_ms;
		}
	}
}

int mb_poller_write(uint8_t dev, uint16_t tag, double value)
{
	struct write_req w = {.dev = dev, .tag = tag, .value = value};

	if (iface < 0) {
		return -ENODEV;
	}
	return k_msgq_put(&write_q, &w, K_NO_WAIT) == 0 ? 0 : -ENOBUFS;
}

int mb_poller_start(const struct gw_config *config)
{
	static const enum uart_config_parity parity[] = {
		[GW_PARITY_NONE] = UART_CFG_PARITY_NONE,
		[GW_PARITY_EVEN] = UART_CFG_PARITY_EVEN,
		[GW_PARITY_ODD] = UART_CFG_PARITY_ODD,
	};
	const char name[] = DEVICE_DT_NAME(MODBUS_NODE);
	struct modbus_iface_param param = {
		.mode = MODBUS_MODE_RTU,
		.rx_timeout = (uint32_t)config->rs485.response_timeout_ms * USEC_PER_MSEC,
		.serial = {
			.baud = config->rs485.baud,
			.parity = parity[config->rs485.parity],
			.stop_bits = (config->rs485.stop_bits == 2) ? UART_CFG_STOP_BITS_2
								    : UART_CFG_STOP_BITS_1,
		},
	};
	int idx;
	int ret;

	cfg = config;
	idx = modbus_iface_get_by_name(name);
	if (idx < 0) {
		LOG_ERR("Modbus interface %s not found", name);
		return idx;
	}
	ret = modbus_init_client(idx, param);
	if (ret < 0) {
		LOG_ERR("Modbus client init failed (%d)", ret);
		return ret;
	}
	iface = idx;
	LOG_INF("RS485 %u baud, parity %u, %u stop bit(s), timeout %u ms, %u retries",
		config->rs485.baud, config->rs485.parity, config->rs485.stop_bits,
		config->rs485.response_timeout_ms, config->rs485.retries);

	k_thread_create(&poller_thread, poller_stack, K_THREAD_STACK_SIZEOF(poller_stack),
			poller_main, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	k_thread_name_set(&poller_thread, "mb_poller");
	return 0;
}
