#include "gw_conv.h"

#include <math.h>
#include <string.h>

#include "spb.h"

bool gw_conv_is_scaled(const gw_tag_cfg_t *tag)
{
    return tag->scale != 1.0 || tag->offset != 0.0;
}

uint32_t gw_conv_datatype(const gw_tag_cfg_t *tag)
{
    if (tag->type == GW_TYPE_BOOL) {
        return SPB_DT_BOOLEAN;
    }
    if (gw_conv_is_scaled(tag)) {
        return SPB_DT_FLOAT;
    }
    switch (tag->type) {
    case GW_TYPE_INT16:
        return SPB_DT_INT16;
    case GW_TYPE_UINT16:
        return SPB_DT_UINT16;
    case GW_TYPE_INT32:
        return SPB_DT_INT32;
    case GW_TYPE_UINT32:
        return SPB_DT_UINT32;
    default:
        return SPB_DT_FLOAT;
    }
}

static uint32_t join32(const gw_tag_cfg_t *tag, const uint16_t *regs)
{
    return tag->word_order == GW_WORD_BIG ? ((uint32_t)regs[0] << 16) | regs[1]
                                          : ((uint32_t)regs[1] << 16) | regs[0];
}

static void split32(const gw_tag_cfg_t *tag, uint32_t v, uint16_t regs[2])
{
    uint16_t hi = (uint16_t)(v >> 16);
    uint16_t lo = (uint16_t)v;
    regs[0] = tag->word_order == GW_WORD_BIG ? hi : lo;
    regs[1] = tag->word_order == GW_WORD_BIG ? lo : hi;
}

double gw_conv_from_regs(const gw_tag_cfg_t *tag, const uint16_t *regs)
{
    double raw;
    switch (tag->type) {
    case GW_TYPE_INT16:
        raw = (int16_t)regs[0];
        break;
    case GW_TYPE_UINT32:
        raw = join32(tag, regs);
        break;
    case GW_TYPE_INT32:
        raw = (int32_t)join32(tag, regs);
        break;
    case GW_TYPE_FLOAT32: {
        uint32_t bits = join32(tag, regs);
        float f;
        memcpy(&f, &bits, sizeof(f));
        raw = f;
        break;
    }
    default:
        raw = regs[0];
        break;
    }
    return raw * tag->scale + tag->offset;
}

gw_conv_status_t gw_conv_to_regs(const gw_tag_cfg_t *tag, double value, uint16_t regs[2])
{
    if (!isfinite(value)) {
        return GW_CONV_NOT_FINITE;
    }
    double raw = (value - tag->offset) / tag->scale;
    if (tag->type == GW_TYPE_FLOAT32) {
        if (fabs(raw) > 3.4028234663852886e38) {
            return GW_CONV_RANGE;
        }
        float f = (float)raw;
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        split32(tag, bits, regs);
        return GW_CONV_OK;
    }
    double r = round(raw);
    switch (tag->type) {
    case GW_TYPE_INT16:
        if (r < -32768.0 || r > 32767.0) {
            return GW_CONV_RANGE;
        }
        regs[0] = (uint16_t)(int16_t)r;
        return GW_CONV_OK;
    case GW_TYPE_UINT16:
        if (r < 0.0 || r > 65535.0) {
            return GW_CONV_RANGE;
        }
        regs[0] = (uint16_t)r;
        return GW_CONV_OK;
    case GW_TYPE_INT32:
        if (r < -2147483648.0 || r > 2147483647.0) {
            return GW_CONV_RANGE;
        }
        split32(tag, (uint32_t)(int32_t)r, regs);
        return GW_CONV_OK;
    case GW_TYPE_UINT32:
        if (r < 0.0 || r > 4294967295.0) {
            return GW_CONV_RANGE;
        }
        split32(tag, (uint32_t)r, regs);
        return GW_CONV_OK;
    default:
        return GW_CONV_RANGE;  /* bool tags are written as coils, never as registers */
    }
}
