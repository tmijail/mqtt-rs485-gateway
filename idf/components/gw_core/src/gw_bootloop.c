#include "gw_bootloop.h"

#include "gw_nvsraw.h"

static uint32_t crc_of(const gw_bootloop_t *r)
{
    uint8_t b[8];
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(r->magic >> (8 * i));
        b[4 + i] = (uint8_t)(r->count >> (8 * i));
    }
    return gw_nvsraw_crc32(0, b, sizeof(b));
}

static bool valid(const gw_bootloop_t *r)
{
    return r->magic == GW_BOOTLOOP_MAGIC && r->crc == crc_of(r);
}

void gw_bootloop_set(gw_bootloop_t *r, uint32_t count)
{
    r->magic = GW_BOOTLOOP_MAGIC;
    r->count = count;
    r->crc = crc_of(r);
}

uint32_t gw_bootloop_count(const gw_bootloop_t *r)
{
    return valid(r) ? r->count : 0;
}

uint32_t gw_bootloop_boot(gw_bootloop_t *r, gw_reset_kind_t why, bool pending)
{
    uint32_t count = gw_bootloop_count(r);
    switch (why) {
    case GW_RESET_POWERON:
    case GW_RESET_BROWNOUT:
    case GW_RESET_PWR_GLITCH:
        count = 0;
        break;
    case GW_RESET_PANIC:
    case GW_RESET_INT_WDT:
    case GW_RESET_TASK_WDT:
    case GW_RESET_WDT:
    case GW_RESET_CPU_LOCKUP:
        if (!pending && count < UINT32_MAX) {
            count++;
        }
        break;
    default:
        break;   /* a planned restart, the reset button, the others */
    }
    gw_bootloop_set(r, count);
    return count;
}

bool gw_bootloop_safe(uint32_t count, bool pending)
{
    return !pending && count >= GW_BOOTLOOP_SAFE;
}
