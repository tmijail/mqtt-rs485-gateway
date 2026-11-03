/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gw_time.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>
#include <zephyr/sys/clock.h>

LOG_MODULE_REGISTER(gw_time, CONFIG_GW_LOG_LEVEL);

#define EVT_SYNCED BIT(0)

static K_EVENT_DEFINE(time_events);

int64_t gw_time_now_ms(void)
{
	struct timespec ts;

	if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
		return k_uptime_get();
	}
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool gw_time_synced(void)
{
	return k_event_test(&time_events, EVT_SYNCED) != 0;
}

bool gw_time_wait_synced(k_timeout_t timeout)
{
	return k_event_wait(&time_events, EVT_SYNCED, false, timeout) != 0;
}

int gw_time_sntp_sync(const char *server)
{
	struct sntp_time st;
	struct timespec ts;
	int ret;

	ret = sntp_simple(server, CONFIG_GW_SNTP_TIMEOUT_MS, &st);
	if (ret < 0) {
		LOG_WRN("SNTP %s failed (%d)", server, ret);
		return ret;
	}
	ts.tv_sec = (time_t)st.seconds;
	ts.tv_nsec = (long)(((uint64_t)st.fraction * 1000000000ULL) >> 32);
	ret = sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
	if (ret < 0) {
		LOG_ERR("cannot set the clock (%d)", ret);
		return ret;
	}
	if (!gw_time_synced()) {
		LOG_INF("clock synchronised with %s (unix %llu)", server,
			(unsigned long long)st.seconds);
	}
	k_event_post(&time_events, EVT_SYNCED);
	return 0;
}
