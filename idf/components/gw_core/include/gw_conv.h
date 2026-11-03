/*
 * Conversion between Modbus registers and engineering values (MB-5, MB-7) and
 * the tag type -> Sparkplug datatype mapping of SPEC section 6.
 *
 * Engineering values are carried as double: every MVP type (up to 32 bits and
 * float32) round-trips exactly through a double.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_CONV_OK = 0,
    GW_CONV_NOT_FINITE,   /* NaN or infinity requested */
    GW_CONV_RANGE,        /* raw value does not fit the register type */
} gw_conv_status_t;

/** true when scale/offset alter the raw value (the metric is then published as Float). */
bool gw_conv_is_scaled(const gw_tag_cfg_t *tag);

/** Sparkplug datatype of the tag's metric. */
uint32_t gw_conv_datatype(const gw_tag_cfg_t *tag);

/** Engineering value from 1 or 2 registers (holding/input tags): raw * scale + offset. */
double gw_conv_from_regs(const gw_tag_cfg_t *tag, const uint16_t *regs);

/**
 * Registers to write for engineering value @p value: (value - offset) / scale,
 * rounded to nearest for integer types, range-checked, word order applied.
 * Writes gw_tag_width(tag) registers into @p regs.
 */
gw_conv_status_t gw_conv_to_regs(const gw_tag_cfg_t *tag, double value, uint16_t regs[2]);

#ifdef __cplusplus
}
#endif
