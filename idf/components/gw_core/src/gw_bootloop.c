#include "gw_bootloop.h"

#include "gw_nvsraw.h"

static uint32_t crc_of(const gw_bootloop_t *r)
{
    uint8_t b[12];
    for (int i = 0; i < 4; i++) {
        b[i] = (uint8_t)(r->magic >> (8 * i));
        b[4 + i] = (uint8_t)(r->count >> (8 * i));
        b[8 + i] = (uint8_t)(r->request >> (8 * i));
    }
    return gw_nvsraw_crc32(0, b, sizeof(b));
}

static bool valid(const gw_bootloop_t *r)
{
    return r->magic == GW_BOOTLOOP_MAGIC && r->crc == crc_of(r);
}

static void store(gw_bootloop_t *r, uint32_t count, bool request)
{
    r->magic = GW_BOOTLOOP_MAGIC;
    r->count = count;
    r->request = request ? GW_BOOTLOOP_REQUEST : 0u;
    r->crc = crc_of(r);
}

static bool requested(const gw_bootloop_t *r)
{
    return valid(r) && r->request == GW_BOOTLOOP_REQUEST;
}

void gw_bootloop_set(gw_bootloop_t *r, uint32_t count)
{
    store(r, count, requested(r));
}

void gw_bootloop_request(gw_bootloop_t *r)
{
    store(r, gw_bootloop_count(r), true);
}

bool gw_bootloop_take_request(gw_bootloop_t *r)
{
    bool req = requested(r);
    if (req) {
        store(r, r->count, false);
    }
    return req;
}

uint32_t gw_bootloop_count(const gw_bootloop_t *r)
{
    return valid(r) ? r->count : 0;
}

uint32_t gw_bootloop_boot(gw_bootloop_t *r, gw_reset_kind_t why, bool pending)
{
    uint32_t count = gw_bootloop_count(r);
    bool request = requested(r);
    switch (why) {
    case GW_RESET_POWERON:
    case GW_RESET_BROWNOUT:
    case GW_RESET_PWR_GLITCH:
        count = 0;
        request = false;   /* issue #209: a power cut before the restart: no request */
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
    store(r, count, request);
    return count;
}

bool gw_bootloop_safe(uint32_t count, bool pending)
{
    return !pending && count >= GW_BOOTLOOP_SAFE;
}
