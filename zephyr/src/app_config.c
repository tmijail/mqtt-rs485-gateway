/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_config.h"

#include <errno.h>

#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_config, CONFIG_GW_LOG_LEVEL);

/* Default configuration, embedded at build time from GW_CONFIG_FILE. */
static const char default_config[] = {
#include "default_config.json.inc"
	0x00};

/* Load buffer, only used once at boot. */
static char file_buf[CONFIG_GW_CONFIG_MAX_SIZE];

static int read_file(const char *path, size_t *len)
{
	struct fs_dirent entry;
	struct fs_file_t file;
	ssize_t n;
	int ret;

	ret = fs_stat(path, &entry);
	if (ret < 0) {
		return ret;
	}
	if (entry.size > sizeof(file_buf)) {
		LOG_ERR("%s: %u bytes exceeds the %u byte limit", path, (unsigned int)entry.size,
			(unsigned int)sizeof(file_buf));
		return -EFBIG;
	}
	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_READ);
	if (ret < 0) {
		return ret;
	}
	n = fs_read(&file, file_buf, entry.size);
	(void)fs_close(&file);
	if (n != (ssize_t)entry.size) {
		return (n < 0) ? (int)n : -EIO;
	}
	*len = (size_t)n;
	return 0;
}

int app_config_load(struct gw_config *cfg, const char **source)
{
	char err[GW_CFG_ERR_MAX];
	const char *doc = default_config;
	size_t len = sizeof(default_config) - 1;
	int ret;

	*source = "built-in default";
	ret = read_file(CONFIG_GW_CONFIG_PATH, &len);
	if (ret == 0) {
		doc = file_buf;
		*source = CONFIG_GW_CONFIG_PATH;
	} else if (ret == -ENOENT) {
		len = sizeof(default_config) - 1;
	} else {
		LOG_ERR("cannot read %s (%d)", CONFIG_GW_CONFIG_PATH, ret);
		return ret;
	}

	ret = gw_config_parse(doc, len, cfg, err, sizeof(err));
	if (ret < 0) {
		LOG_ERR("invalid configuration (%s): %s", *source, err);
		return ret;
	}
	LOG_INF("configuration from %s: %u device(s), %u tag(s)", *source, cfg->device_count,
		cfg->tag_count);
	return 0;
}
