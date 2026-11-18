/*
 * Issue #226: a microSD card in RAM for the spill of store & forward (gw_sfspill_io_t), shared by
 * fuzz_sfspill and by fuzz_seeds (which writes card images with the real spill).
 *
 * The ring has CARD_EXTENTS extents of CARD_EXT_BLOCKS blocks. A card image (the input of
 * fuzz_sfspill) is a control byte, then the two superblock copies (512 bytes each) and the
 * extents, in that order. Bits 0-3 of the control byte: the files ringa.sf, ringb.sf, r0000.sf
 * and r0001.sf are on the card; bit 4: a segment file of the step 1 format too; bit 5: correct
 * the CRC of each superblock copy; bit 6: correct the CRC of each block (the fuzzer can then
 * change the fields behind the CRCs); bit 7: the card's allocation unit is 4 KiB (else unknown).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gw_sfspill.h"

#define CARD_EXT_BLOCKS 2u
#define CARD_EXTENTS    2u
#define CARD_EXT_BYTES  (CARD_EXT_BLOCKS * GW_SFSPILL_BLOCK)
#define CARD_LEGACY_ID  5u    /* a step 1 segment "00000005.sf" */
#define CARD_FILES      5     /* ringa, ringb, r0000, r0001, the segment */
#define CARD_IMAGE_SIZE (1u + 2u * GW_SFSPILL_SB_BYTES + CARD_EXTENTS * CARD_EXT_BYTES)

typedef struct {
    int32_t size;             /* -1: absent */
    uint32_t cap;
    uint8_t *data;
} card_file_t;

typedef struct {
    card_file_t f[CARD_FILES];
    uint8_t sb_mem[2][GW_SFSPILL_SB_BYTES];
    uint8_t ext_mem[CARD_EXTENTS][CARD_EXT_BYTES];
    uint8_t legacy_mem[512];
    uint32_t ops;
} card_t;

static inline card_file_t *card_file(card_t *c, uint32_t id)
{
    uint32_t kind = id & GW_SFSPILL_F_KIND, i = id & ~GW_SFSPILL_F_KIND;
    if (kind == GW_SFSPILL_F_SUPER) {
        return i < 2 ? &c->f[i] : NULL;
    }
    if (kind == GW_SFSPILL_F_EXTENT) {
        return i < CARD_EXTENTS ? &c->f[2 + i] : NULL;
    }
    return id == CARD_LEGACY_ID ? &c->f[4] : NULL;
}

static inline uint32_t card_id(int index)
{
    if (index < 2) {
        return GW_SFSPILL_F_SUPER | (uint32_t)index;
    }
    if (index < 4) {
        return GW_SFSPILL_F_EXTENT | (uint32_t)(index - 2);
    }
    return CARD_LEGACY_ID;
}

static int card_alloc(void *ctx, uint32_t id, uint32_t size)
{
    card_t *c = ctx;
    card_file_t *m = card_file(c, id);
    c->ops++;
    if (m == NULL || size > m->cap) {
        return -1;
    }
    if ((int32_t)size > m->size) {
        int32_t from = m->size < 0 ? 0 : m->size;
        memset(m->data + from, 0, size - (uint32_t)from);
        m->size = (int32_t)size;
    }
    return 1;
}

static bool card_write(void *ctx, uint32_t id, uint32_t off, const void *d, size_t len)
{
    card_t *c = ctx;
    card_file_t *m = card_file(c, id);
    c->ops++;
    if (m == NULL || m->size < 0 || off + len > (uint32_t)m->size) {
        return false;
    }
    memcpy(m->data + off, d, len);
    return true;
}

static int card_read(void *ctx, uint32_t id, uint32_t off, void *d, size_t len)
{
    card_t *c = ctx;
    card_file_t *m = card_file(c, id);
    c->ops++;
    if (m == NULL || m->size < 0 || off + len > (uint32_t)m->size) {
        return 0;
    }
    memcpy(d, m->data + off, len);
    return 1;
}

static bool card_truncate(void *ctx, uint32_t id, uint32_t len)
{
    card_t *c = ctx;
    card_file_t *m = card_file(c, id);
    c->ops++;
    if (m == NULL || m->size < 0) {
        return false;
    }
    if ((int32_t)len < m->size) {
        m->size = (int32_t)len;
    }
    return true;
}

static bool card_remove(void *ctx, uint32_t id)
{
    card_t *c = ctx;
    card_file_t *m = card_file(c, id);
    c->ops++;
    if (m != NULL) {
        m->size = -1;
    }
    return true;
}

static int32_t card_size(void *ctx, uint32_t id)
{
    card_t *c = ctx;
    card_file_t *m = card_file(c, id);
    return m == NULL ? -1 : m->size;
}

static bool card_list(void *ctx, void (*each)(void *arg, uint32_t f), void *arg)
{
    card_t *c = ctx;
    for (int i = 0; i < CARD_FILES; i++) {
        if (c->f[i].size >= 0) {
            each(arg, card_id(i));
        }
    }
    return true;
}

/* An empty card. */
static inline void card_init(card_t *c)
{
    memset(c, 0, sizeof(*c));
    for (int i = 0; i < 2; i++) {
        c->f[i] = (card_file_t){-1, GW_SFSPILL_SB_BYTES, c->sb_mem[i]};
    }
    for (unsigned i = 0; i < CARD_EXTENTS; i++) {
        c->f[2 + i] = (card_file_t){-1, CARD_EXT_BYTES, c->ext_mem[i]};
    }
    c->f[4] = (card_file_t){-1, sizeof(c->legacy_mem), c->legacy_mem};
}

static inline gw_sfspill_io_t card_io(card_t *c, uint32_t unit)
{
    return (gw_sfspill_io_t){c, unit, card_alloc, card_write, card_read, card_truncate,
                             card_remove, card_size, card_list};
}

/* The card of an image (see above); a short image leaves the rest zero. Returns the unit. */
static inline uint32_t card_load(card_t *c, const uint8_t *img, size_t len)
{
    card_init(c);
    uint8_t ctl = len > 0 ? img[0] : 0;
    size_t pos = 1;
    uint8_t *dst[4] = {c->sb_mem[0], c->sb_mem[1], c->ext_mem[0], c->ext_mem[1]};
    size_t sz[4] = {GW_SFSPILL_SB_BYTES, GW_SFSPILL_SB_BYTES, CARD_EXT_BYTES, CARD_EXT_BYTES};
    for (int i = 0; i < 4; i++) {
        size_t n = pos < len ? (len - pos < sz[i] ? len - pos : sz[i]) : 0;
        if (n != 0) {
            memcpy(dst[i], img + pos, n);
        }
        pos += sz[i];
        if ((ctl >> i) & 1u) {
            c->f[i].size = (int32_t)sz[i];
        }
    }
    if (ctl & 16u) {
        c->f[4].size = 0;
    }
    if (ctl & 32u) {
        for (int i = 0; i < 2; i++) {
            gw_sfspill_sb_t s;
            memcpy(&s, c->sb_mem[i], sizeof(s));
            s.crc = gw_sfspill_crc32c(0, &s, offsetof(gw_sfspill_sb_t, crc));
            memcpy(c->sb_mem[i], &s, sizeof(s));
        }
    }
    if (ctl & 64u) {
        for (unsigned e = 0; e < CARD_EXTENTS; e++) {
            for (unsigned b = 0; b < CARD_EXT_BLOCKS; b++) {
                uint8_t *blk = c->ext_mem[e] + b * GW_SFSPILL_BLOCK;
                gw_sfspill_blk_t h;
                memcpy(&h, blk, sizeof(h));
                size_t nrec = h.nrec <= GW_SFSPILL_BLK_RECS ? h.nrec : 0;
                h.crc = 0;
                uint32_t crc = gw_sfspill_crc32c(0, &h, sizeof(h));
                h.crc = gw_sfspill_crc32c(crc, blk + sizeof(h), nrec * sizeof(gw_sf_rec_t));
                memcpy(blk, &h, sizeof(h));
            }
        }
    }
    return (ctl & 128u) != 0 ? 4096u : 0u;
}

/* The image of a card: the files that are there, whole, and no correction of a CRC. */
static inline size_t card_save(const card_t *c, uint8_t *img, size_t cap)
{
    if (cap < CARD_IMAGE_SIZE) {
        return 0;
    }
    uint8_t ctl = 0;
    for (int i = 0; i < 4; i++) {
        if (c->f[i].size >= 0) {
            ctl |= (uint8_t)(1u << i);
        }
    }
    img[0] = ctl;
    size_t pos = 1;
    memcpy(img + pos, c->sb_mem, sizeof(c->sb_mem));
    pos += sizeof(c->sb_mem);
    memcpy(img + pos, c->ext_mem, sizeof(c->ext_mem));
    pos += sizeof(c->ext_mem);
    return pos;
}
