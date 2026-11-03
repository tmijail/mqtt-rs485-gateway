/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "local_io.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "debounce.h"
#include "gw_time.h"
#include "model.h"
#include "status_led.h"

LOG_MODULE_REGISTER(local_io, CONFIG_GW_LOG_LEVEL);

#define USER_NODE DT_PATH(zephyr_user)

#define SPEC_DI(node, prop, idx) GPIO_DT_SPEC_GET_BY_IDX(node, prop, idx)

static const struct gpio_dt_spec di[] = {
	DT_FOREACH_PROP_ELEM_SEP(USER_NODE, di_gpios, SPEC_DI, (,))};
static const struct gpio_dt_spec dout[] = {
	DT_FOREACH_PROP_ELEM_SEP(USER_NODE, do_gpios, SPEC_DI, (,))};

BUILD_ASSERT(ARRAY_SIZE(di) == GW_DI_COUNT, "zephyr,user di-gpios must list 8 inputs");
BUILD_ASSERT(ARRAY_SIZE(dout) == GW_DO_COUNT, "zephyr,user do-gpios must list 8 outputs");

#define SAMPLE_PERIOD_MS 1  /* 1 kHz sampling (IO-1) */
#define LED_PERIOD_MS    50

K_THREAD_STACK_DEFINE(io_stack, 1536);
static struct k_thread io_thread;
static struct k_timer sample_timer;
static struct debounce db;
static K_MUTEX_DEFINE(out_lock);
static bool outputs_ready;
static uint8_t out_mask; /* last state written to the outputs */

static uint8_t read_inputs(void)
{
	uint8_t raw = 0;

	for (size_t i = 0; i < ARRAY_SIZE(di); i++) {
		if (gpio_pin_get_dt(&di[i]) > 0) {
			raw |= (uint8_t)BIT(i);
		}
	}
	return raw;
}

static void io_main(void *p1, void *p2, void *p3)
{
	int wdt = task_wdt_add(CONFIG_GW_WDT_TIMEOUT_MS, NULL, NULL);
	uint32_t ticks = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_timer_start(&sample_timer, K_MSEC(SAMPLE_PERIOD_MS), K_MSEC(SAMPLE_PERIOD_MS));
	while (true) {
		uint8_t flipped;

		(void)k_timer_status_sync(&sample_timer);
		flipped = debounce_step(&db, read_inputs());
		if (flipped != 0) {
			model_lock();
			model_io()->di = db.state;
			model_io()->di_ts_ms = gw_time_now_ms();
			model_unlock();
			model_signal_node();
		}
		if (++ticks >= LED_PERIOD_MS / SAMPLE_PERIOD_MS) {
			ticks = 0;
			status_led_update(k_uptime_get());
			if (wdt >= 0) {
				(void)task_wdt_feed(wdt);
			}
		}
	}
}

int local_io_set_output(uint8_t channel, bool on)
{
	uint8_t mask;
	int ret;

	if (channel >= ARRAY_SIZE(dout)) {
		return -EINVAL;
	}
	if (!outputs_ready) {
		return -ENODEV;
	}
	/* The expander acknowledging the I2C write is the confirmation. */
	k_mutex_lock(&out_lock, K_FOREVER);
	ret = gpio_pin_set_dt(&dout[channel], on ? 1 : 0);
	if (ret == 0) {
		WRITE_BIT(out_mask, channel, on);
	}
	mask = out_mask;
	k_mutex_unlock(&out_lock);
	if (ret < 0) {
		LOG_ERR("DO/%u: I/O error %d", channel + 1, ret);
		return ret;
	}

	model_lock();
	model_io()->outputs = mask;
	model_io()->do_ts_ms = gw_time_now_ms();
	model_unlock();
	model_signal_node();
	LOG_INF("DO/%u set %s", channel + 1, on ? "ON" : "OFF");
	return 0;
}

const struct gpio_dt_spec *local_io_di_spec(uint8_t channel)
{
	return (channel < ARRAY_SIZE(di)) ? &di[channel] : NULL;
}

static int init_outputs(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(dout); i++) {
		int ret;

		if (!gpio_is_ready_dt(&dout[i])) {
			return -ENODEV;
		}
		/* Outputs start OFF on every boot. */
		ret = gpio_pin_configure_dt(&dout[i], GPIO_OUTPUT_INACTIVE);
		if (ret < 0) {
			return ret;
		}
	}
	return 0;
}

int local_io_init(void)
{
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(di); i++) {
		if (!gpio_is_ready_dt(&di[i])) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&di[i], GPIO_INPUT);
		if (ret < 0) {
			return ret;
		}
	}
	ret = init_outputs();
	outputs_ready = (ret == 0);
	if (!outputs_ready) {
		/* Inputs and the status LED keep working; NCMD DO/n will fail. */
		LOG_ERR("digital outputs unavailable (%d)", ret);
	}

	model_lock();
	model_io()->outputs = 0;
	model_io()->di = read_inputs();
	model_io()->di_ts_ms = gw_time_now_ms();
	model_io()->do_ts_ms = model_io()->di_ts_ms;
	model_unlock();
	return 0;
}

int local_io_start(uint16_t debounce_ms)
{
	model_lock();
	debounce_init(&db, model_io()->di, debounce_ms / SAMPLE_PERIOD_MS);
	model_unlock();

	k_timer_init(&sample_timer, NULL, NULL);
	k_thread_create(&io_thread, io_stack, K_THREAD_STACK_SIZEOF(io_stack), io_main, NULL,
			NULL, NULL, K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
	k_thread_name_set(&io_thread, "local_io");
	return 0;
}
