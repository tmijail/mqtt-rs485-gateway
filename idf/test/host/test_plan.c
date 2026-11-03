/* Block read planning (MB-11). */
#include <stdio.h>
#include <string.h>

#include "gw_plan.h"
#include "unity.h"

static gw_config_t cfg;
static gw_plan_t plan;

void setUp(void)
{
    memset(&cfg, 0, sizeof(cfg));
}
void tearDown(void) {}

static void add_tag(gw_area_t area, gw_type_t type, uint16_t addr)
{
    gw_tag_cfg_t *t = &cfg.tags[cfg.tag_count];
    snprintf(t->name, sizeof(t->name), "t%u", cfg.tag_count);
    t->area = area;
    t->type = type;
    t->address = addr;
    t->scale = 1;
    cfg.tag_count++;
    cfg.devices[cfg.device_count - 1].tag_count++;
}

static void add_device(void)
{
    cfg.devices[cfg.device_count].first_tag = cfg.tag_count;
    cfg.device_count++;
}

static void test_merges_contiguous_and_overlapping(void)
{
    add_device();
    add_tag(GW_AREA_HOLDING, GW_TYPE_FLOAT32, 10);  /* 10-11 */
    add_tag(GW_AREA_HOLDING, GW_TYPE_UINT16, 0);    /* 0 */
    add_tag(GW_AREA_HOLDING, GW_TYPE_UINT16, 1);    /* 1 */
    add_tag(GW_AREA_HOLDING, GW_TYPE_INT16, 11);    /* overlaps the float's low word */
    add_tag(GW_AREA_INPUT, GW_TYPE_UINT16, 1);      /* other area: own block */
    add_tag(GW_AREA_COIL, GW_TYPE_BOOL, 7);
    add_tag(GW_AREA_COIL, GW_TYPE_BOOL, 5);
    gw_plan_build(&plan, &cfg, true, 0);
    TEST_ASSERT_EQUAL(5, plan.block_count);
    /* sorted by area: coil 5, coil 7 (gap 1 > 0), holding 0-1, holding 10-11, input 1 */
    TEST_ASSERT_EQUAL(GW_AREA_COIL, plan.blocks[0].area);
    TEST_ASSERT_EQUAL(5, plan.blocks[0].start);
    TEST_ASSERT_EQUAL(1, plan.blocks[0].qty);
    TEST_ASSERT_EQUAL(GW_AREA_HOLDING, plan.blocks[2].area);
    TEST_ASSERT_EQUAL(0, plan.blocks[2].start);
    TEST_ASSERT_EQUAL(2, plan.blocks[2].qty);
    TEST_ASSERT_EQUAL(2, plan.blocks[2].member_count);
    TEST_ASSERT_EQUAL(10, plan.blocks[3].start);
    TEST_ASSERT_EQUAL(2, plan.blocks[3].qty);
    TEST_ASSERT_EQUAL(2, plan.blocks[3].member_count);
    TEST_ASSERT_EQUAL(GW_AREA_INPUT, plan.blocks[4].area);

    gw_plan_build(&plan, &cfg, true, 8);  /* bridges 2..9 and the coil hole */
    TEST_ASSERT_EQUAL(3, plan.block_count);
    TEST_ASSERT_EQUAL(3, plan.blocks[0].qty);   /* coils 5..7 */
    TEST_ASSERT_EQUAL(12, plan.blocks[1].qty);  /* holding 0..11 */
    TEST_ASSERT_EQUAL(4, plan.blocks[1].member_count);

    gw_plan_build(&plan, &cfg, false, 8);
    TEST_ASSERT_EQUAL(7, plan.block_count);
}

static void test_block_size_limit_and_devices(void)
{
    add_device();
    for (uint16_t i = 0; i < 64; i++) {
        add_tag(GW_AREA_INPUT, GW_TYPE_UINT32, (uint16_t)(i * 2));  /* 128 contiguous registers */
    }
    add_device();
    add_tag(GW_AREA_INPUT, GW_TYPE_UINT16, 0);
    gw_plan_build(&plan, &cfg, true, 0);
    TEST_ASSERT_EQUAL(2, plan.dev_block_count[0]);
    TEST_ASSERT_EQUAL(124, plan.blocks[0].qty);  /* 62 uint32 fit in 125 registers */
    TEST_ASSERT_EQUAL(62, plan.blocks[0].member_count);
    TEST_ASSERT_EQUAL(124, plan.blocks[1].start);
    TEST_ASSERT_EQUAL(4, plan.blocks[1].qty);
    TEST_ASSERT_EQUAL(1, plan.dev_block_count[1]);
    TEST_ASSERT_EQUAL(2, plan.dev_first_block[1]);
    TEST_ASSERT_EQUAL(64, plan.members[plan.blocks[2].first_member]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_merges_contiguous_and_overlapping);
    RUN_TEST(test_block_size_limit_and_devices);
    return UNITY_END();
}
