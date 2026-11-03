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
    uint16_t fail_streak;
} gw_devstate_t;

/**
 * Account for one poll cycle. A cycle succeeds when the device answered at
 * least one request (exception responses count as answers). The device goes
 * offline after @p offline_after consecutive failed cycles and back online on
 * the first successful one.
 */
gw_dev_event_t gw_devstate_cycle(gw_devstate_t *st, bool success, uint16_t offline_after);

#ifdef __cplusplus
}
#endif
