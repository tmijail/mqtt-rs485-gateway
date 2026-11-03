/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bdseq.h"

#include <errno.h>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(bdseq, CONFIG_GW_LOG_LEVEL);

#define KEY_ROOT "gw"
#define KEY_NAME "bdseq"

static uint8_t stored;

static int bdseq_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, KEY_NAME, &next) && next == NULL) {
		if (len != sizeof(stored)) {
			return -EINVAL;
		}
		return read_cb(cb_arg, &stored, sizeof(stored)) == sizeof(stored) ? 0 : -EIO;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(gw_bdseq, KEY_ROOT, NULL, bdseq_set, NULL, NULL);

int bdseq_init(void)
{
	int ret = settings_subsys_init();

	if (ret == 0) {
		ret = settings_load_subtree(KEY_ROOT);
	}
	if (ret < 0) {
		LOG_ERR("settings unavailable (%d): bdSeq restarts at 0", ret);
		return ret;
	}
	LOG_INF("stored bdSeq %u", stored);
	return 0;
}

uint8_t bdseq_next(void)
{
	uint8_t current = stored;
	uint8_t next = (uint8_t)(current + 1U);
	int ret = settings_save_one(KEY_ROOT "/" KEY_NAME, &next, sizeof(next));

	if (ret < 0) {
		LOG_ERR("cannot persist bdSeq (%d)", ret);
	}
	stored = next;
	return current;
}
