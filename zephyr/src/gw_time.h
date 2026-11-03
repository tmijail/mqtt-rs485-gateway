/*
 * Wall clock for Sparkplug timestamps (TIME-1).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_TIME_H_
#define GW_TIME_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/** Milliseconds since the Unix epoch (UTC). Before the first SNTP sync this
 *  counts from 1970-01-01 at boot, see gw_time_synced().
 */
int64_t gw_time_now_ms(void);

/** True once the clock has been set by SNTP at least once. */
bool gw_time_synced(void);

/** Wait until the clock has been synchronised, at most @p timeout. */
bool gw_time_wait_synced(k_timeout_t timeout);

/** Query @p server over SNTP and set the system realtime clock. */
int gw_time_sntp_sync(const char *server);

#endif /* GW_TIME_H_ */
