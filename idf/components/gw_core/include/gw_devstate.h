/* Modbus device online/offline tracking (MB-9). Devices start offline. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_DEV_NO_CHANGE = 0,
    GW_DEV_WENT_ONLINE,
    GW_DEV_WENT_OFFLINE,
} gw_dev_event_t;

typedef struct {
    bool online;
    bool seen;              /* has been online at least once since boot */
    uint16_t fail_streak;
} gw_devstate_t;

/**
 * true while the device must only be probed (see gw_poll.h): offline after offline_after failed
 * cycles, or never online since boot. A device that never answered is offline for Sparkplug
 * (no DBIRTH yet) and gets no full cycles with retries: several absent units would otherwise
 * saturate the bus for offline_after cycles after every boot (issue #61).
 */
bool gw_devstate_probing(const gw_devstate_t *st, uint16_t offline_after);

/**
 * Account for one poll cycle. A cycle succeeds when the device answered every
 * request of the cycle (exception responses count as answers). The device goes
 * offline after @p offline_after consecutive failed cycles and back online on
 * the first successful one.
 */
gw_dev_event_t gw_devstate_cycle(gw_devstate_t *st, bool success, uint16_t offline_after);

#ifdef __cplusplus
}
#endif
