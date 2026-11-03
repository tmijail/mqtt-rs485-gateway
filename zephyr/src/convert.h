/*
 * Modbus register <-> engineering value conversion (MB-5, MB-7) and the
 * tag type to Sparkplug datatype mapping (SPEC section 6).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_CONVERT_H_
#define GW_CONVERT_H_

#include <stdint.h>

#include "gw_config.h"

/** True if the tag applies a linear transformation (scale != 1 or offset != 0). */
static inline bool gw_tag_is_scaled(const struct gw_tag_cfg *t)
{
	return t->scale != 1.0 || t->offset != 0.0;
}

/** Sparkplug datatype (Tahu DataType value) published for a tag. */
uint32_t gw_tag_datatype(const struct gw_tag_cfg *t);

/**
 * Decode one or two holding/input registers into the tag's engineering
 * value: value = raw * scale + offset.
 */
double gw_regs_to_value(const struct gw_tag_cfg *t, const uint16_t regs[2]);

/**
 * Inverse conversion for writes: raw = (value - offset) / scale, rounded to
 * the nearest integer for integer types.
 *
 * @retval 0 on success, -ERANGE if the raw value does not fit the type.
 */
int gw_value_to_regs(const struct gw_tag_cfg *t, double value, uint16_t regs[2]);

#endif /* GW_CONVERT_H_ */
