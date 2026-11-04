#include "gw_nvsraw.h"

#include <string.h>

#define HDR_LEN      32u
#define BITMAP_LEN   32u
#define ENTRY_LEN    32u
#define ENTRY_COUNT  126u
#define ST_ACTIVE    0xFFFFFFFEu
#define ST_FULL      0xFFFFFFFCu
#define ST_FREEING   0xFFFFFFF8u
#define ES_WRITTEN   2u

uint32_t gw_nvsraw_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void gw_nvsraw_scan_init(gw_nvsraw_scan_t *s)
{
    memset(s, 0, sizeof(*s));
}

/* Item CRC of NVS (Item::calculateCrc32): nsIndex..chunkIndex, key, data; not the crc field. */
static bool item_ok(const uint8_t *e)
{
    uint32_t c = gw_nvsraw_crc32(0xFFFFFFFFu, e, 4);
    c = gw_nvsraw_crc32(c, e + 8, 16);
    c = gw_nvsraw_crc32(c, e + 24, 8);
    return c == le32(e + 4);
}

void gw_nvsraw_scan_page(gw_nvsraw_scan_t *s, const uint8_t *page)
{
    uint32_t state = le32(page);
    if (state != ST_ACTIVE && state != ST_FULL && state != ST_FREEING) {
        return;   /* uninitialised, corrupt or invalid: nothing NVS would read */
    }
    if (gw_nvsraw_crc32(0xFFFFFFFFu, page + 4, 24) != le32(page + 28)) {
        return;   /* not an NVS page header */
    }
    s->pages++;
    const uint8_t *bitmap = page + HDR_LEN;
    const uint8_t *entries = page + HDR_LEN + BITMAP_LEN;
    uint32_t i = 0;
    while (i < ENTRY_COUNT) {
        uint32_t st = (le32(bitmap + (i / 16u) * 4u) >> ((i % 16u) * 2u)) & 3u;
        if (st != ES_WRITTEN) {
            i++;
            continue;
        }
        const uint8_t *e = entries + i * ENTRY_LEN;
        s->written++;
        if (item_ok(e)) {
            s->plain_ok++;
            uint32_t span = e[2];
            if (span == 0 || i + span > ENTRY_COUNT) {
                span = 1;
            }
            s->written += span - 1;   /* its data entries */
            i += span;
        } else {
            s->bad++;
            i++;
        }
    }
}

gw_nvsraw_kind_t gw_nvsraw_verdict(const gw_nvsraw_scan_t *s)
{
    if (s->written == 0) {
        return GW_NVSRAW_EMPTY;
    }
    return s->plain_ok > 0 && s->plain_ok >= s->bad ? GW_NVSRAW_PLAIN : GW_NVSRAW_ENCRYPTED;
}
