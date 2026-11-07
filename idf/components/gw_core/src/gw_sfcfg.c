/*
 * SPB-14 (#145): the configuration hash of the store & forward spill.
 *
 * A record on the card holds a device index, a global tag index and a value (gw_sf_rec_t). The
 * replay (gw_node_flush_history) turns it into a metric through the configuration of the boot
 * that replays it: the device at that index (its name is the device_id, its template the
 * instance metric), the tag at that index (alias GW_ALIAS_TAG_BASE + index, its name for a
 * template member) and the tag's Sparkplug datatype, which also selects how the value union of
 * the record is read (gw_conv_kind is a function of it, see the host test). The hash covers
 * exactly those fields, fed one by one with a fixed size and byte order: a configuration change
 * that keeps them, or a firmware update that changes only the C layout of the structures, keeps
 * the data on the card.
 *
 * Canonical input, format GW_SFCFG_HASH_FORMAT (FNV-1a over the bytes below, in this order;
 * integers little-endian, a string is its length as u16 then its bytes, no NUL):
 *   u32 format
 *   str sparkplug.group_id, str sparkplug.edge_node_id      (the topic of every replayed message)
 *   u16 device_count
 *   per device:  str name, str type (template), u16 first_tag, u16 tag_count
 *   u16 tag_count
 *   per tag:     str name, u32 Sparkplug datatype, u8 bytes_len (length of a Bytes tag, else 0)
 *
 * Left out, because the record does not depend on them (the stored value is the engineering
 * value, already scaled and converted, and the replay never reads these):
 *   tag:    area, address, bit, word_order, slow, writable, deadband, units, eng_low/high and
 *           their flags; scale and offset (only whether a tag is scaled matters, and the
 *           datatype carries it: Int16 -> Float, Int64 -> Double); length of a String tag (the
 *           record holds the text with its own length)
 *   device: unit_id, poll_ms, slow_poll_ms, offline_after, block_reads, max_gap
 *   node metrics (GW_SF_NODE: DI, DO, counters): fixed aliases and datatypes, no configuration
 *   everything else (network, MQTT, broker list, primary host, rs485, local_io, web, ...)
 */
#include <string.h>

#include "gw_conv.h"
#include "gw_sfspill.h"
#include "spb.h"

/*
 * Guard (#145): a new field in gw_tag_cfg_t or gw_device_cfg_t needs a decision. These
 * positional initializers name every field in order; with -Wextra -Werror (gw_core builds with
 * both, on the host and on the target) a new field fails the build with "missing initializer"
 * (-Wmissing-field-initializers), and a removed field with "excess elements". The check counts
 * fields, not bytes, so it holds for any compiler, padding and size of an enum.
 * When it fails:
 *   1. Decide whether the new field changes what a stored record means on replay: the metric it
 *      becomes (device, name, alias, template) or how its value is decoded (the datatype).
 *   2. If it does, feed it in gw_sfspill_cfg_hash() below, increment GW_SFCFG_HASH_FORMAT and
 *      update the golden value in test_sfcfg.c; if not, add it to the "Left out" list above.
 *   3. Add the new field to the initializer below.
 */
#define GW_SFCFG_HASH_FORMAT 1u

#ifdef GW_SFCFG_GUARD_SELFTEST
/* test/host (sfcfg_guard): one initializer short, as after a new field: the build must fail */
#define GW_SFCFG_LAST_TAG_FIELD
#else
#define GW_SFCFG_LAST_TAG_FIELD 0.0f, /* eng_high */
#endif

static const gw_tag_cfg_t s_tag_fields = {
    "",           /* name: hashed */
    GW_AREA_COIL, /* area */
    GW_TYPE_BOOL, /* type: hashed through the datatype */
    GW_WORD_BIG,  /* word_order */
    0,            /* address */
    GW_NO_BIT,    /* bit */
    0,            /* length: hashed for a Bytes tag */
    false,        /* slow */
    false,        /* writable */
    false,        /* has_eng_low */
    false,        /* has_eng_high */
    1.0,          /* scale: hashed through the datatype */
    0.0,          /* offset: hashed through the datatype */
    0.0,          /* deadband */
    "",           /* units */
    0.0f,         /* eng_low */
    GW_SFCFG_LAST_TAG_FIELD
};

static const gw_device_cfg_t s_device_fields = {
    "", /* name: hashed */
    "", /* type: hashed */
    0,  /* unit_id */
    0,  /* poll_ms */
    0,  /* slow_poll_ms */
    0,  /* offline_after */
    0,  /* block_reads */
    0,  /* max_gap */
    0,  /* first_tag: hashed */
    0,  /* tag_count: hashed */
};

static uint32_t feed_u8(uint32_t h, uint8_t v)
{
    return gw_sfspill_fnv(h, &v, 1);
}

static uint32_t feed_u16(uint32_t h, uint16_t v)
{
    const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    return gw_sfspill_fnv(h, b, sizeof(b));
}

static uint32_t feed_u32(uint32_t h, uint32_t v)
{
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return gw_sfspill_fnv(h, b, sizeof(b));
}

/* The text up to its NUL (at most @p cap bytes): the bytes after it never count. */
static uint32_t feed_str(uint32_t h, const char *s, size_t cap)
{
    size_t n = strnlen(s, cap);
    return gw_sfspill_fnv(feed_u16(h, (uint16_t)n), s, n);
}

uint32_t gw_sfspill_cfg_hash(const gw_config_t *cfg)
{
    (void)s_tag_fields;
    (void)s_device_fields;
    uint32_t h = feed_u32(GW_SFSPILL_FNV_INIT, GW_SFCFG_HASH_FORMAT);
    h = feed_str(h, cfg->sparkplug.group_id, sizeof(cfg->sparkplug.group_id));
    h = feed_str(h, cfg->sparkplug.edge_node_id, sizeof(cfg->sparkplug.edge_node_id));
    h = feed_u16(h, cfg->device_count);
    for (size_t i = 0; i < cfg->device_count; i++) {
        const gw_device_cfg_t *d = &cfg->devices[i];
        h = feed_str(h, d->name, sizeof(d->name));
        h = feed_str(h, d->type, sizeof(d->type));
        h = feed_u16(h, d->first_tag);
        h = feed_u16(h, d->tag_count);
    }
    h = feed_u16(h, cfg->tag_count);
    for (size_t i = 0; i < cfg->tag_count; i++) {
        const gw_tag_cfg_t *t = &cfg->tags[i];
        uint32_t dt = gw_conv_datatype(t);
        h = feed_str(h, t->name, sizeof(t->name));
        h = feed_u32(h, dt);
        h = feed_u8(h, dt == SPB_DT_BYTES ? t->length : 0);
    }
    return h;
}
