#include "gw_devstate.h"

gw_dev_event_t gw_devstate_cycle(gw_devstate_t *st, bool success, uint16_t offline_after)
{
    if (success) {
        st->fail_streak = 0;
        if (!st->online) {
            st->online = true;
            return GW_DEV_WENT_ONLINE;
        }
        return GW_DEV_NO_CHANGE;
    }
    if (st->fail_streak < UINT16_MAX) {
        st->fail_streak++;
    }
    if (st->online && st->fail_streak >= offline_after) {
        st->online = false;
        return GW_DEV_WENT_OFFLINE;
    }
    return GW_DEV_NO_CHANGE;
}
