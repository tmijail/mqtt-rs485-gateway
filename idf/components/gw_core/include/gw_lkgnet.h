/*
 * CFG-3 / FW-1 (issue #144 item 8): the network of the last valid configuration ("lkg_net" in
 * NVS), in a format that tolerates new fields, pure C (host-tested).
 *
 * Recovery mode, safe mode and commissioning start the network with it, so that a site with a
 * static IP stays reachable. An earlier image reads it after a rollback, so it is not a raw
 * struct: "GWLN", a format version, then one record per field: tag (1 byte), length (1 byte),
 * value. A reader takes the tags that it knows (a value of another length is skipped) and
 * ignores the others; a field that is not there keeps its default (DHCP). The firmware writes it
 * only from a confirmed image in normal mode.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_LKG_MAGIC "GWLN"
#define GW_LKG_VERSION 1u
#define GW_LKG_MAX 320   /* the largest record set (all strings at their maximum) */

/** The network and the web ports of @p cfg as records; returns the length, 0 when @p cap is too
 *  small. */
size_t gw_lkg_encode(const gw_config_t *cfg, uint8_t *buf, size_t cap);

/** Puts the fields of @p buf into @p cfg (network and web ports only; the others stay). False
 *  when @p buf is not a record set (no magic, a cut record). */
bool gw_lkg_decode(const uint8_t *buf, size_t len, gw_config_t *cfg);

#ifdef __cplusplus
}
#endif
