/*
 * Conversion between Modbus registers and engineering values (MB-5, MB-6, MB-7) and
 * the tag type -> Sparkplug datatype mapping of SPEC section 6.
 *
 * Engineering values are carried in a gw_tag_value_t whose member in use follows the tag
 * (gw_conv_kind): a double for every type up to 32 bits, float32/float64 and any scaled tag
 * (all exact in a double up to 32 bits); the exact 64-bit integer for unscaled int64/uint64
 * (a double is not exact above 2^53); the text for strings.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"
#include "gw_tagdb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_CONV_OK = 0,
    GW_CONV_NOT_FINITE,   /* NaN or infinity requested */
    GW_CONV_RANGE,        /* raw value does not fit the register type (or the string its length) */
} gw_conv_status_t;

/** Member of gw_tag_value_t that carries the tag's value. */
typedef enum {
    GW_VK_DOUBLE = 0,   /* .value */
    GW_VK_INT64,        /* .i64: unscaled int64 */
    GW_VK_UINT64,       /* .u64: unscaled uint64 */
    GW_VK_STRING,       /* .str */
    GW_VK_BYTES,        /* .str holds exactly 2 x length raw bytes (SPB-19) */
} gw_val_kind_t;

/** true when scale/offset alter the raw value (the metric is then published as Float/Double). */
bool gw_conv_is_scaled(const gw_tag_cfg_t *tag);

gw_val_kind_t gw_conv_kind(const gw_tag_cfg_t *tag);

/** Sparkplug datatype of the tag's metric. */
uint32_t gw_conv_datatype(const gw_tag_cfg_t *tag);

/**
 * Decode gw_tag_width(tag) registers of a holding/input tag (not a register bit) into
 * @p out (member per gw_conv_kind), applying byte order, scale and offset.
 */
void gw_conv_decode(const gw_tag_cfg_t *tag, const uint16_t *regs, gw_tag_value_t *out);

/** Engineering value of a double-kind tag (convenience over gw_conv_decode). */
double gw_conv_from_regs(const gw_tag_cfg_t *tag, const uint16_t *regs);

/**
 * Registers to write for value @p in (member per gw_conv_kind): (value - offset) / scale,
 * rounded to nearest for integer types, range-checked, byte order applied; strings are
 * padded with NUL. Writes gw_tag_width(tag) registers into @p regs.
 */
gw_conv_status_t gw_conv_encode(const gw_tag_cfg_t *tag, const gw_tag_value_t *in, uint16_t *regs);

/** gw_conv_encode() from a double (an integral, in-range value for unscaled 64-bit tags). */
gw_conv_status_t gw_conv_to_regs(const gw_tag_cfg_t *tag, double value, uint16_t *regs);

/** Store @p value in the member the tag uses; false if it does not fit (strings never do). */
bool gw_conv_from_double(const gw_tag_cfg_t *tag, double value, gw_tag_value_t *out);

/** The tag's value as a double (approximate above 2^53; 0 for strings), for logs and deadbands. */
double gw_conv_as_double(const gw_tag_cfg_t *tag, const gw_tag_value_t *v);

/** Value as text for logs ("12.3", "-9007199254740993", "'SN-42'"). */
void gw_conv_format(const gw_tag_cfg_t *tag, const gw_tag_value_t *v, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
