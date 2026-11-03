/*
 * Configuration source selection (CFG-2): the file in LittleFS overrides
 * the default document embedded in the firmware.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_APP_CONFIG_H_
#define GW_APP_CONFIG_H_

#include "gw_config.h"

/**
 * Load the active configuration.
 *
 * @param source set to a description of where it came from.
 * @retval 0 on success, -EINVAL if the selected document is invalid (the
 *         error has been logged), another negative errno on I/O errors.
 */
int app_config_load(struct gw_config *cfg, const char **source);

#endif /* GW_APP_CONFIG_H_ */
