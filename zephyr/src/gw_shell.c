/*
 * "gw" shell commands: status and diagnostics (subset of CFG-6).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>

#include "gw_time.h"
#include "local_io.h"
#include "model.h"
#include "spb_node.h"

#if defined(CONFIG_GPIO_EMUL)
#include <zephyr/drivers/gpio/gpio_emul.h>
#endif

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	const struct gw_config *cfg = model_cfg();
	uint32_t st = model_status();
	uint8_t di;
	uint8_t outputs;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "config: %s, network: %s, mqtt: %s, time: %s, seq: %u",
		    (st & GW_STATUS_CONFIG_ERROR) ? "INVALID" : "ok",
		    (st & GW_STATUS_NET_UP) ? "up" : "down",
		    (st & GW_STATUS_MQTT_UP) ? "connected" : "disconnected",
		    gw_time_synced() ? "synced" : "not synced", spb_node_seq());
	if (st & GW_STATUS_CONFIG_ERROR) {
		return 0;
	}

	model_lock();
	di = model_io()->di;
	outputs = model_io()->outputs;
	model_unlock();
	shell_print(sh, "DI 0x%02x  DO 0x%02x (bit n = channel n+1)", di, outputs);

	for (uint8_t i = 0; i < cfg->device_count; i++) {
		struct gw_dev_state ds;

		model_lock();
		ds = *model_dev(i);
		model_unlock();
		shell_print(sh, "%-16s unit %3u %-7s ok %u timeout %u error %u", cfg->devices[i].name,
			    cfg->devices[i].unit_id, ds.online ? "online" : "offline", ds.ok_count,
			    ds.timeout_count, ds.error_count);
	}
	return 0;
}

#if defined(CONFIG_GPIO_EMUL)
/* Simulator only: drive an emulated digital input (logical level). */
static int cmd_di(const struct shell *sh, size_t argc, char **argv)
{
	long ch = strtol(argv[1], NULL, 10);
	long level = strtol(argv[2], NULL, 10);
	const struct gpio_dt_spec *spec;
	int phys;

	ARG_UNUSED(argc);
	spec = local_io_di_spec((uint8_t)(ch - 1));
	if (ch < 1 || spec == NULL || (level != 0 && level != 1)) {
		shell_error(sh, "usage: gw di <1-8> <0|1>");
		return -EINVAL;
	}
	phys = (spec->dt_flags & GPIO_ACTIVE_LOW) ? !level : level;
	return gpio_emul_input_set(spec->port, spec->pin, phys);
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(gw_cmds,
	SHELL_CMD(status, NULL, "Gateway, device and I/O status", cmd_status),
#if defined(CONFIG_GPIO_EMUL)
	SHELL_CMD_ARG(di, NULL, "Set emulated input: di <1-8> <0|1>", cmd_di, 3, 0),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gw, &gw_cmds, "RS485 Sparkplug gateway", NULL);
