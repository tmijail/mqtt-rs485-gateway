/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "convert.h"
#include "sparkplug.h"

#include <errno.h>
#include <math.h>
#include <string.h>

uint32_t gw_tag_datatype(const struct gw_tag_cfg *t)
{
	if (t->type == GW_TYPE_BOOL) {
		return SPB_DT_BOOLEAN;
	}
	if (gw_tag_is_scaled(t)) {
		return SPB_DT_FLOAT;
	}
	switch (t->type) {
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

static uint32_t join32(const struct gw_tag_cfg *t, const uint16_t regs[2])
{
	return (t->word_order == GW_WORD_BIG) ? ((uint32_t)regs[0] << 16) | regs[1]
					      : ((uint32_t)regs[1] << 16) | regs[0];
}

static void split32(const struct gw_tag_cfg *t, uint32_t v, uint16_t regs[2])
{
	uint16_t hi = (uint16_t)(v >> 16);
	uint16_t lo = (uint16_t)v;

	regs[0] = (t->word_order == GW_WORD_BIG) ? hi : lo;
	regs[1] = (t->word_order == GW_WORD_BIG) ? lo : hi;
}

double gw_regs_to_value(const struct gw_tag_cfg *t, const uint16_t regs[2])
{
	double raw;

	switch (t->type) {
	case GW_TYPE_INT16:
		raw = (int16_t)regs[0];
		break;
	case GW_TYPE_UINT32:
		raw = join32(t, regs);
		break;
	case GW_TYPE_INT32:
		raw = (int32_t)join32(t, regs);
		break;
	case GW_TYPE_FLOAT32: {
		uint32_t bits = join32(t, regs);
		float f;

		memcpy(&f, &bits, sizeof(f));
		raw = f;
		break;
	}
	default: /* uint16 and bool (coil/discrete bit already in regs[0]) */
		raw = regs[0];
		break;
	}
	return raw * t->scale + t->offset;
}

int gw_value_to_regs(const struct gw_tag_cfg *t, double value, uint16_t regs[2])
{
	double raw = (value - t->offset) / t->scale;
	double r;

	if (!isfinite(raw)) {
		return -ERANGE;
	}
	if (t->type == GW_TYPE_FLOAT32) {
		float f = (float)raw;
		uint32_t bits;

		if (!isfinite(f)) {
			return -ERANGE;
		}
		memcpy(&bits, &f, sizeof(bits));
		split32(t, bits, regs);
		return 0;
	}

	r = round(raw);
	switch (t->type) {
	case GW_TYPE_BOOL:
		regs[0] = (r != 0.0) ? 1U : 0U;
		return 0;
	case GW_TYPE_UINT16:
		if (r < 0.0 || r > 65535.0) {
			return -ERANGE;
		}
		regs[0] = (uint16_t)r;
		return 0;
	case GW_TYPE_INT16:
		if (r < -32768.0 || r > 32767.0) {
			return -ERANGE;
		}
		regs[0] = (uint16_t)(int16_t)r;
		return 0;
	case GW_TYPE_UINT32:
		if (r < 0.0 || r > 4294967295.0) {
			return -ERANGE;
		}
		split32(t, (uint32_t)r, regs);
		return 0;
	case GW_TYPE_INT32:
		if (r < -2147483648.0 || r > 2147483647.0) {
			return -ERANGE;
		}
		split32(t, (uint32_t)(int32_t)r, regs);
		return 0;
	default:
		return -ERANGE;
	}
}
