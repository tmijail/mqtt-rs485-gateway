/*
 * RS485 (Modbus RTU master) to MQTT Sparkplug B 3.0 edge node.
 *
 * Boot sequence: watchdog, configuration, local I/O, persistent state,
 * network, RS485 bus owner, MQTT session. With an invalid configuration
 * the gateway does not poll nor connect (CFG-1); only the status LED and
 * the shell stay alive so the configuration can be replaced.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>

#include <zephyr/app_version.h>

#include "app_config.h"
#include "bdseq.h"
#include "local_io.h"
#include "mb_poller.h"
#include "model.h"
#include "mqtt_link.h"
#include "netmgr.h"

LOG_MODULE_REGISTER(main, CONFIG_GW_LOG_LEVEL);

static struct gw_config config;

static void log_reset_cause(void)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) == 0) {
		LOG_INF("reset cause 0x%08x", cause);
		(void)hwinfo_clear_reset_cause();
	}
}

static int start_watchdog(void)
{
	const struct device *hw_wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));

	if (hw_wdt != NULL && !device_is_ready(hw_wdt)) {
		hw_wdt = NULL;
	}
	/* The hardware watchdog backs up the task watchdog itself (REL-1). */
	return task_wdt_init(hw_wdt);
}

int main(void)
{
	const char *source;
	int ret;

	LOG_INF("RS485 Sparkplug B gateway %s (%s)", APP_VERSION_STRING,
		STRINGIFY(APP_BUILD_VERSION));
	log_reset_cause();

	ret = start_watchdog();
	if (ret < 0) {
		LOG_ERR("task watchdog init failed (%d)", ret);
	}

	ret = app_config_load(&config, &source);
	model_init(&config);

	if (local_io_init() < 0) {
		LOG_ERR("digital inputs unavailable");
	}
	(void)local_io_start(ret == 0 ? config.local_io.di_debounce_ms : 20);

	if (ret < 0) {
		model_status_set(GW_STATUS_CONFIG_ERROR, true);
		LOG_ERR("not starting: fix the configuration and reboot");
		return 0;
	}

	(void)bdseq_init();
	if (netmgr_start(&config) < 0) {
		LOG_ERR("network start failed");
	}
	if (mb_poller_start(&config) < 0) {
		LOG_ERR("RS485 start failed");
	}
	if (mqtt_link_start(&config) < 0) {
		LOG_ERR("MQTT start failed");
	}
	return 0;
}
