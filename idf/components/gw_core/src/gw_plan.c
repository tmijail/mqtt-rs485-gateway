#include "gw_plan.h"

#include <string.h>

#include "mb_rtu.h"

static bool tag_before(const gw_tag_cfg_t *a, const gw_tag_cfg_t *b)
{
    return a->area != b->area ? a->area < b->area : a->address < b->address;
}

void gw_plan_build(gw_plan_t *plan, const gw_config_t *cfg, bool merge, uint16_t max_gap)
{
    memset(plan, 0, sizeof(*plan));
    uint16_t nm = 0;
    for (uint16_t d = 0; d < cfg->device_count; d++) {
        const gw_device_cfg_t *dev = &cfg->devices[d];
        uint16_t *order = &plan->members[nm];

        /* Insertion sort of the device's tags by (area, address); at most 64 entries. */
        for (uint16_t i = 0; i < dev->tag_count; i++) {
            uint16_t t = (uint16_t)(dev->first_tag + i);
            uint16_t j = i;
            while (j > 0 && tag_before(&cfg->tags[t], &cfg->tags[order[j - 1]])) {
                order[j] = order[j - 1];
                j--;
            }
            order[j] = t;
        }

        plan->dev_first_block[d] = plan->block_count;
        bool dev_merge = dev->block_reads == GW_CFG_BUILD_DEFAULT ? merge : dev->block_reads != 0;
        uint32_t dev_gap = dev->max_gap == GW_CFG_BUILD_DEFAULT ? max_gap : (uint32_t)dev->max_gap;
        gw_block_t *blk = NULL;
        for (uint16_t i = 0; i < dev->tag_count; i++) {
            const gw_tag_cfg_t *t = &cfg->tags[order[i]];
            uint32_t t_end = (uint32_t)t->address + gw_tag_width(t);  /* exclusive */
            if (blk != NULL && dev_merge && blk->area == t->area) {
                uint32_t b_end = (uint32_t)blk->start + blk->qty;
                uint32_t limit = (t->area == GW_AREA_COIL || t->area == GW_AREA_DISCRETE)
                                     ? MB_MAX_READ_BITS
                                     : MB_MAX_READ_REGS;
                uint32_t new_end = t_end > b_end ? t_end : b_end;
                if (t->address <= b_end + dev_gap && new_end - blk->start <= limit) {
                    blk->qty = (uint16_t)(new_end - blk->start);
                    blk->member_count++;
                    continue;
                }
            }
            blk = &plan->blocks[plan->block_count++];
            blk->area = t->area;
            blk->start = t->address;
            blk->qty = (uint16_t)(t_end - t->address);
            blk->first_member = (uint16_t)(nm + i);
            blk->member_count = 1;
        }
        plan->dev_block_count[d] = (uint16_t)(plan->block_count - plan->dev_first_block[d]);
        nm = (uint16_t)(nm + dev->tag_count);
    }
}
