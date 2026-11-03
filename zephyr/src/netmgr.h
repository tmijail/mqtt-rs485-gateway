/*
 * Network interface management and time synchronisation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_NETMGR_H_
#define GW_NETMGR_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "gw_config.h"

/** Configure addressing (DHCP/static, DNS) and start the SNTP thread. */
int netmgr_start(const struct gw_config *cfg);

/** Wait until the network is usable (link up and an IPv4 address). */
bool netmgr_wait_ready(k_timeout_t timeout);

#endif /* GW_NETMGR_H_ */
