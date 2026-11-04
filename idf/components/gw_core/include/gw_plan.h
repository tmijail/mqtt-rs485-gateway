/*
 * Read planning: groups a device's tags into Modbus read requests (MB-11).
 * Tags of the same area whose addresses are contiguous, overlapping or separated
 * by at most `max_gap` unused registers/bits are read with one request, up to 125
 * registers or 2000 bits. With merging disabled every tag gets its own request.
 * Merging and the gap are per device (block_reads, max_gap); @p merge and @p max_gap of
 * gw_plan_build() are the build defaults for devices that do not set them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gw_area_t area;
    uint16_t start;
    uint16_t qty;
    uint16_t first_member;   /* index into gw_plan_t.members */
    uint16_t member_count;
} gw_block_t;

typedef struct {
    gw_block_t blocks[GW_MAX_TAGS];
    uint16_t members[GW_MAX_TAGS];   /* global tag indices, grouped per block */
    uint16_t dev_first_block[GW_MAX_DEVICES];
    uint16_t dev_block_count[GW_MAX_DEVICES];
    uint16_t block_count;
} gw_plan_t;

void gw_plan_build(gw_plan_t *plan, const gw_config_t *cfg, bool merge, uint16_t max_gap);

#ifdef __cplusplus
}
#endif
