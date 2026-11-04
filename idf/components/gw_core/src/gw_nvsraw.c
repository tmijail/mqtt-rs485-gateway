#include "gw_nvsraw.h"

#include <string.h>

#define HDR_LEN      32u
#define BITMAP_LEN   32u
#define ENTRY_LEN    32u
#define ENTRY_COUNT  126u
#define ST_ACTIVE    0xFFFFFFFEu
#define ST_FULL      0xFFFFFFFCu
#define ST_FREEING   0xFFFFFFF8u
#define ES_WRITTEN   2u

uint32_t gw_nvsraw_crc32(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void gw_nvsraw_scan_init(gw_nvsraw_scan_t *s)
{
    memset(s, 0, sizeof(*s));
}

/* Item CRC of NVS (Item::calculateCrc32): nsIndex..chunkIndex, key, data; not the crc field. */
static bool item_ok(const uint8_t *e)
{
    uint32_t c = gw_nvsraw_crc32(0xFFFFFFFFu, e, 4);
    c = gw_nvsraw_crc32(c, e + 8, 16);
    c = gw_nvsraw_crc32(c, e + 24, 8);
    return c == le32(e + 4);
}

/* A page NVS would read: active, full or freeing, with a valid header CRC (not uninitialised,
 * corrupt or invalid). */
static bool page_ok(const uint8_t *page)
{
    uint32_t state = le32(page);
    if (state != ST_ACTIVE && state != ST_FULL && state != ST_FREEING) {
        return false;
    }
    return gw_nvsraw_crc32(0xFFFFFFFFu, page + 4, 24) == le32(page + 28);
}

/* Entry @p i of @p page if the state bitmap marks it written, else NULL. */
static const uint8_t *written_entry(const uint8_t *page, uint32_t i)
{
    const uint8_t *bitmap = page + HDR_LEN;
    uint32_t st = (le32(bitmap + (i / 16u) * 4u) >> ((i % 16u) * 2u)) & 3u;
    return st == ES_WRITTEN ? page + HDR_LEN + BITMAP_LEN + i * ENTRY_LEN : NULL;
}

/* Entries an item at index @p i takes (its own and its data), at least 1. */
static uint32_t item_span(const uint8_t *e, uint32_t i)
{
    uint32_t span = e[2];
    return span == 0 || i + span > ENTRY_COUNT ? 1u : span;
}

/* Issue #107: a header whose CRC failed but that still reads as a plain variable-length item (a
 * string or a blob, its span matching its declared size): one damaged item, not as many as the
 * entries it takes. Its data entries are skipped like those of a good item. 0 otherwise. */
#define TYPE_STR       0x21u
#define TYPE_BLOB      0x41u
#define TYPE_BLOB_DATA 0x42u

static uint32_t damaged_span(const uint8_t *e, uint32_t i)
{
    if (e[1] != TYPE_STR && e[1] != TYPE_BLOB && e[1] != TYPE_BLOB_DATA) {
        return 0;
    }
    uint32_t size = (uint32_t)e[24] | (uint32_t)e[25] << 8;
    uint32_t span = e[2];
    return span >= 2u && i + span <= ENTRY_COUNT && span == 1u + (size + ENTRY_LEN - 1u) / ENTRY_LEN
               ? span
               : 0;
}

void gw_nvsraw_scan_page(gw_nvsraw_scan_t *s, const uint8_t *page)
{
    if (!page_ok(page)) {
        return;   /* uninitialised, corrupt or invalid: nothing NVS would read */
    }
    s->pages++;
    uint32_t ok = 0, bad = 0;
    for (uint32_t i = 0; i < ENTRY_COUNT;) {
        const uint8_t *e = written_entry(page, i);
        if (e == NULL) {
            i++;
            continue;
        }
        s->written++;
        if (item_ok(e)) {
            ok++;
            uint32_t span = item_span(e, i);
            s->written += span - 1;   /* its data entries */
            i += span;
        } else {
            bad++;   /* one per item: a damaged string or blob is not one per entry (#107) */
            uint32_t span = damaged_span(e, i);
            if (span > 1u) {
                s->written += span - 1u;
                i += span;
            } else {
                i++;
            }
        }
    }
    s->plain_ok += ok;
    s->bad += bad;
    if (ok > 0 && ok >= bad) {
        s->plain_pages++;
    } else if (bad > 0) {
        s->enc_pages++;
    }
}

gw_nvsraw_kind_t gw_nvsraw_verdict(const gw_nvsraw_scan_t *s)
{
    if (s->written == 0) {
        return GW_NVSRAW_EMPTY;
    }
    if (s->plain_pages > 0 && s->enc_pages > 0) {
        return GW_NVSRAW_MIXED;
    }
    return s->plain_pages > 0 ? GW_NVSRAW_PLAIN : GW_NVSRAW_ENCRYPTED;
}

/* ---------- a u8 item from the raw pages (issue #94) ---------- */

#define TYPE_U8 0x01u

static void copy_name(char *dst, const char *src)
{
    size_t n = strlen(src);
    n = n < GW_NVSRAW_NAME - 1u ? n : GW_NVSRAW_NAME - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool key_is(const uint8_t *e, const char *key)
{
    return strncmp((const char *)e + 8, key, GW_NVSRAW_NAME) == 0;
}

void gw_nvsraw_u8_init(gw_nvsraw_u8_t *f, const char *ns, const char *key)
{
    memset(f, 0, sizeof(*f));
    copy_name(f->ns, ns);
    copy_name(f->key, key);
    f->ns_index = -1;
}

/* Calls @p fn for every plain item with a valid CRC of a valid page. */
static void each_item(const uint8_t *page, void (*fn)(gw_nvsraw_u8_t *, const uint8_t *, uint32_t,
                                                      uint32_t),
                      gw_nvsraw_u8_t *f)
{
    if (!page_ok(page)) {
        return;
    }
    uint32_t seq = le32(page + 4);
    for (uint32_t i = 0; i < ENTRY_COUNT;) {
        const uint8_t *e = written_entry(page, i);
        if (e == NULL || !item_ok(e)) {
            i++;
            continue;
        }
        fn(f, e, seq, i);
        i += item_span(e, i);
    }
}

static void on_ns(gw_nvsraw_u8_t *f, const uint8_t *e, uint32_t seq, uint32_t i)
{
    (void)seq;
    (void)i;
    if (e[0] == 0 && e[1] == TYPE_U8 && key_is(e, f->ns) && e[24] != 0) {
        f->ns_index = e[24];
    }
}

static void on_item(gw_nvsraw_u8_t *f, const uint8_t *e, uint32_t seq, uint32_t i)
{
    if (f->ns_index < 0 || e[0] != (uint8_t)f->ns_index || e[1] != TYPE_U8 || !key_is(e, f->key)) {
        return;
    }
    if (!f->found || seq > f->seq || (seq == f->seq && i >= f->entry)) {
        f->found = true;
        f->value = e[24];
        f->seq = seq;
        f->entry = i;
    }
}

void gw_nvsraw_u8_ns(gw_nvsraw_u8_t *f, const uint8_t *page)
{
    each_item(page, on_ns, f);
}

void gw_nvsraw_u8_item(gw_nvsraw_u8_t *f, const uint8_t *page)
{
    each_item(page, on_item, f);
}

/* ---------- the persistent copy of a migration (issue #94) ---------- */

#define MIG_VERSION 2u

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

size_t gw_nvsmig_begin(uint8_t *buf, size_t cap)
{
    if (cap < GW_NVSMIG_HDR) {
        return 0;
    }
    memset(buf, 0, GW_NVSMIG_HDR);
    return GW_NVSMIG_HDR;
}

static bool name_ok(const char *s)
{
    size_t n = strnlen(s, GW_NVSRAW_NAME);
    return n > 0 && n < GW_NVSRAW_NAME;
}

size_t gw_nvsmig_add(uint8_t *buf, size_t cap, size_t used, const char *ns, const char *key,
                     uint8_t type, const void *data, size_t len)
{
    if (used < GW_NVSMIG_HDR || !name_ok(ns) || !name_ok(key) || len > 0xFFFFu ||
        used > cap || cap - used < GW_NVSMIG_REC_HDR + len) {
        return 0;
    }
    uint8_t *r = buf + used;
    memset(r, 0, 2 * GW_NVSRAW_NAME);
    memcpy(r, ns, strlen(ns));
    memcpy(r + GW_NVSRAW_NAME, key, strlen(key));
    r[32] = type;
    put16(r + 33, (uint32_t)len);
    if (len > 0) {
        memcpy(r + GW_NVSMIG_REC_HDR, data, len);
    }
    return used + GW_NVSMIG_REC_HDR + len;
}

bool gw_nvsmig_next(const uint8_t *buf, size_t size, size_t *off, gw_nvsmig_rec_t *r)
{
    size_t o = *off == 0 ? GW_NVSMIG_HDR : *off;
    if (o >= size || size - o < GW_NVSMIG_REC_HDR) {
        return false;
    }
    const uint8_t *p = buf + o;
    size_t len = le16(p + 33);
    if (size - o - GW_NVSMIG_REC_HDR < len || !name_ok((const char *)p) ||
        !name_ok((const char *)p + GW_NVSRAW_NAME)) {
        return false;
    }
    r->ns = (const char *)p;
    r->key = (const char *)p + GW_NVSRAW_NAME;
    r->type = p[32];
    r->len = (uint16_t)len;
    r->data = p + GW_NVSMIG_REC_HDR;
    *off = o + GW_NVSMIG_REC_HDR + len;
    return true;
}

size_t gw_nvsmig_seal(uint8_t *buf, size_t used, uint16_t skipped)
{
    uint32_t count = 0;
    gw_nvsmig_rec_t r;
    for (size_t off = 0; gw_nvsmig_next(buf, used, &off, &r);) {
        count++;
    }
    put32(buf, GW_NVSMIG_MAGIC);
    put16(buf + 4, MIG_VERSION);
    put16(buf + 6, count);
    put32(buf + 8, (uint32_t)(used - GW_NVSMIG_HDR));
    put32(buf + 12, gw_nvsraw_crc32(0, buf + GW_NVSMIG_HDR, used - GW_NVSMIG_HDR));
    put16(buf + 16, skipped);
    put16(buf + 18, 0);
    return used;
}

uint32_t gw_nvsmig_crc(const uint8_t *buf)
{
    return le32(buf + 12);
}

uint16_t gw_nvsmig_skipped(const uint8_t *buf)
{
    return (uint16_t)le16(buf + 16);
}

bool gw_nvsmig_check(const uint8_t *buf, size_t size, uint32_t *count)
{
    if (size < GW_NVSMIG_HDR || le32(buf) != GW_NVSMIG_MAGIC || le16(buf + 4) != MIG_VERSION ||
        le32(buf + 8) != size - GW_NVSMIG_HDR ||
        le32(buf + 12) != gw_nvsraw_crc32(0, buf + GW_NVSMIG_HDR, size - GW_NVSMIG_HDR)) {
        return false;
    }
    uint32_t n = 0;
    size_t off = 0;
    gw_nvsmig_rec_t r;
    while (gw_nvsmig_next(buf, size, &off, &r)) {
        n++;
    }
    if ((off != 0 && off != size) || (off == 0 && size != GW_NVSMIG_HDR) || n != le16(buf + 6)) {
        return false;   /* a record that does not end where the body does */
    }
    if (count != NULL) {
        *count = n;
    }
    return true;
}

/* Version 1 (ebc2aa4): 16-byte header (magic, version, count, length, CRC), same records. */
#define MIG_V1_HDR 16u

static bool v1_ok(const uint8_t *buf, size_t size)
{
    if (size < MIG_V1_HDR || le32(buf + 8) != size - MIG_V1_HDR ||
        le32(buf + 12) != gw_nvsraw_crc32(0, buf + MIG_V1_HDR, size - MIG_V1_HDR)) {
        return false;
    }
    /* every record whole, the count right */
    uint32_t n = 0;
    size_t off = MIG_V1_HDR;
    while (off < size) {
        if (size - off < GW_NVSMIG_REC_HDR) {
            return false;
        }
        size_t len = le16(buf + off + 33);
        if (size - off - GW_NVSMIG_REC_HDR < len || !name_ok((const char *)buf + off) ||
            !name_ok((const char *)buf + off + GW_NVSRAW_NAME)) {
            return false;
        }
        off += GW_NVSMIG_REC_HDR + len;
        n++;
    }
    return n == le16(buf + 6);
}

gw_nvsmig_copy_t gw_nvsmig_load(uint8_t *buf, size_t size, size_t cap, size_t *out_size,
                                bool *legacy, uint32_t *count)
{
    *out_size = 0;
    *legacy = false;
    if (size < 6 || le32(buf) != GW_NVSMIG_MAGIC) {
        return GW_NVSMIG_COPY_BAD;   /* not a copy at all (read whole) */
    }
    uint32_t version = le16(buf + 4);
    if (version == MIG_VERSION) {
        if (!gw_nvsmig_check(buf, size, count)) {
            return GW_NVSMIG_COPY_BAD;
        }
        *out_size = size;
        return GW_NVSMIG_COPY_VALID;
    }
    if (version != 1) {
        return GW_NVSMIG_COPY_UNREADABLE;   /* a newer format: kept for the firmware that wrote it */
    }
    if (!v1_ok(buf, size)) {
        return GW_NVSMIG_COPY_BAD;
    }
    size_t grown = size + (GW_NVSMIG_HDR - MIG_V1_HDR);
    if (cap < grown) {
        return GW_NVSMIG_COPY_UNREADABLE;   /* valid, but no room to convert it here */
    }
    memmove(buf + GW_NVSMIG_HDR, buf + MIG_V1_HDR, size - MIG_V1_HDR);
    gw_nvsmig_seal(buf, grown, 0);
    *legacy = true;
    *out_size = grown;
    return gw_nvsmig_check(buf, grown, count) ? GW_NVSMIG_COPY_VALID : GW_NVSMIG_COPY_BAD;
}

bool gw_nvsmig_find(const uint8_t *buf, size_t size, const char *ns, const char *key,
                    gw_nvsmig_rec_t *r)
{
    for (size_t off = 0; gw_nvsmig_next(buf, size, &off, r);) {
        if (strcmp(r->ns, ns) == 0 && strcmp(r->key, key) == 0) {
            return true;
        }
    }
    return false;
}

uint8_t gw_nvsmig_bdseq_later(uint8_t a, uint8_t b)
{
    uint8_t d = (uint8_t)(b - a);
    if (d == 128) {
        return a > b ? a : b;   /* half the ring: undecidable, the same answer either way */
    }
    return d >= 1 && d <= 127 ? b : a;
}

bool gw_nvsmig_may_erase(const gw_nvsmig_read_t *r)
{
    if (!r->init_ok) {
        return r->init_layout_error;
    }
    return r->listed_all && r->skipped == 0;
}

bool gw_nvsmig_bdseq_reached(uint8_t next, uint8_t reserved)
{
    return (uint8_t)(next - reserved) < 128u;
}

bool gw_nvsmig_bdseq_start(bool have_known, uint8_t known, const gw_nvsmig_side_t *side,
                           uint8_t *start, uint8_t *base)
{
    if (!side->have) {
        *start = *base = known;
        return have_known;
    }
    if (!have_known) {
        *start = side->next;
        *base = side->base_known ? side->base : side->next;
        return true;
    }
    /* the same base: the chain continues, however far it went; another one: the later */
    *start = side->base_known && side->base == known ? side->next
                                                     : gw_nvsmig_bdseq_later(known, side->next);
    *base = known;
    return true;
}

uint8_t gw_nvsmig_bdseq_fold(bool have_nvs, uint8_t nvs, const gw_nvsmig_side_t *side)
{
    if (!side->have) {
        return nvs;
    }
    if (!have_nvs || (side->base_known && side->base == nvs)) {
        return side->next;
    }
    return gw_nvsmig_bdseq_later(nvs, side->next);
}

gw_nvsmig_action_t gw_nvsmig_decide(const gw_nvsmig_state_t *s)
{
    if (s->copy == GW_NVSMIG_COPY_UNREADABLE) {
        return GW_NVSMIG_NO_NVS;
    }
    if (s->copy_unknown && ((s->kind != GW_NVSRAW_ENCRYPTED && s->keys) ||
                            (s->kind == GW_NVSRAW_ENCRYPTED && s->mig_busy))) {
        return GW_NVSMIG_NO_NVS;   /* issue #104: the copy that would finish it is out of reach */
    }
    bool copy = s->copy == GW_NVSMIG_COPY_VALID;
    if (!s->raw_ok) {
        if (copy) {
            return GW_NVSMIG_NO_NVS;
        }
        switch (s->kind) {
        case GW_NVSRAW_ENCRYPTED: return GW_NVSMIG_OPEN_ENCRYPTED;
        case GW_NVSRAW_PLAIN: return GW_NVSMIG_STAY_PLAIN;
        default: return GW_NVSMIG_NO_NVS;   /* nothing (or both kinds) readable: unknown */
        }
    }
    switch (s->kind) {
    case GW_NVSRAW_PLAIN:
    case GW_NVSRAW_MIXED:
        if (s->on_trial || !s->can_copy) {
            /* a pending copy means a partly erased partition: never run on it */
            return copy ? GW_NVSMIG_NO_NVS : GW_NVSMIG_STAY_PLAIN;
        }
        return GW_NVSMIG_MIGRATE;
    case GW_NVSRAW_EMPTY:
        if (copy) {
            return GW_NVSMIG_RESUME;
        }
        return s->on_trial ? GW_NVSMIG_STAY_PLAIN : GW_NVSMIG_OPEN_ENCRYPTED;
    case GW_NVSRAW_ENCRYPTED:
    default:
        return copy ? GW_NVSMIG_RESUME : GW_NVSMIG_OPEN_ENCRYPTED;
    }
}
