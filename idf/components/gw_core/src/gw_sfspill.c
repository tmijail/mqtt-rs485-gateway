#include "gw_sfspill.h"

#include <string.h>

#define HDR ((uint32_t)sizeof(gw_sfspill_hdr_t))
#define REC ((uint32_t)sizeof(gw_sf_rec_t))

uint32_t gw_sfspill_fnv(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* Records in segment @p seg according to its size; -1 if it is not one of ours. */
static int32_t seg_records(gw_sfspill_t *sp, uint32_t seg)
{
    int32_t size = sp->io.size(sp->io.ctx, seg);
    gw_sfspill_hdr_t h;
    if (size < (int32_t)HDR || !sp->io.read(sp->io.ctx, seg, 0, &h, sizeof(h)) ||
        h.magic != GW_SFSPILL_MAGIC || h.version != GW_SFSPILL_VERSION || h.rec_size != REC ||
        h.cfg_hash != sp->cfg_hash || h.seg_id != seg) {
        return -1;
    }
    return (int32_t)(((uint32_t)size - HDR) / REC);   /* a partial last record is ignored */
}

static void fail(gw_sfspill_t *sp)
{
    sp->ok = false;
}

void gw_sfspill_init(gw_sfspill_t *sp, const gw_sfspill_io_t *io, uint32_t seg_records_,
                     uint32_t max_segs, uint32_t cfg_hash)
{
    memset(sp, 0, sizeof(*sp));
    sp->io = *io;
    sp->seg_records = seg_records_ == 0 ? 1 : seg_records_;
    sp->max_segs = max_segs < 2 ? 2 : max_segs > GW_SFSPILL_MAX_SEGS ? GW_SFSPILL_MAX_SEGS
                                                                     : max_segs;
    sp->cfg_hash = cfg_hash;
    sp->ok = true;

    /* Keep the newest contiguous run of valid segments, at most max_segs; delete the rest. */
    uint32_t ids[GW_SFSPILL_MAX_SEGS];
    size_t n = io->list(io->ctx, ids, GW_SFSPILL_MAX_SEGS);
    uint32_t top = 0;
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (seg_records(sp, ids[i]) >= 0 && (!any || ids[i] > top)) {
            top = ids[i];
            any = true;
        }
    }
    uint32_t low = top;
    if (any) {
        while (low > 0 && top - low + 1 < sp->max_segs && seg_records(sp, low - 1) >= 0) {
            low--;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (!any || ids[i] < low || ids[i] > top) {
            io->remove(io->ctx, ids[i]);
        }
    }
    if (any) {
        sp->first = low;
        sp->last = top;
        sp->nsegs = top - low + 1;
        for (uint32_t s = low; s <= top; s++) {
            sp->count += (uint32_t)seg_records(sp, s);
        }
        sp->first_count = (uint32_t)seg_records(sp, low);
        sp->last_count = (uint32_t)seg_records(sp, top);
        sp->kept_at_boot = (uint32_t)sp->count;
        sp->next_id = top + 1;
    }
}

static bool new_segment(gw_sfspill_t *sp)
{
    uint32_t id = sp->next_id++;
    if (sp->nsegs >= sp->max_segs) {
        /* full: the oldest segment goes, with what was not replayed of it */
        uint32_t lost = sp->first_count - sp->read_off;
        sp->dropped += lost;
        sp->count -= lost;
        if (!sp->io.remove(sp->io.ctx, sp->first)) {
            fail(sp);
            return false;
        }
        sp->first++;
        sp->nsegs--;
        sp->read_off = 0;
        int32_t c = seg_records(sp, sp->first);
        sp->first_count = c > 0 ? (uint32_t)c : 0;
    }
    sp->io.remove(sp->io.ctx, id);   /* a stale file with that number */
    const gw_sfspill_hdr_t h = {GW_SFSPILL_MAGIC, GW_SFSPILL_VERSION, (uint16_t)REC,
                                sp->cfg_hash, id};
    if (!sp->io.append(sp->io.ctx, id, &h, sizeof(h))) {
        fail(sp);
        return false;
    }
    if (sp->nsegs == 0) {
        sp->first = id;
        sp->first_count = 0;
        sp->read_off = 0;
    }
    sp->last = id;
    sp->last_count = 0;
    sp->nsegs++;
    return true;
}

bool gw_sfspill_append(gw_sfspill_t *sp, const gw_sf_rec_t *recs, size_t n)
{
    while (n > 0 && sp->ok) {
        if ((sp->nsegs == 0 || sp->last_count >= sp->seg_records) && !new_segment(sp)) {
            return false;
        }
        size_t k = sp->seg_records - sp->last_count;
        k = k < n ? k : n;
        if (!sp->io.append(sp->io.ctx, sp->last, recs, k * REC)) {
            fail(sp);
            return false;
        }
        sp->last_count += (uint32_t)k;
        if (sp->first == sp->last) {
            sp->first_count = sp->last_count;
        }
        sp->count += k;
        sp->written += (uint32_t)k;
        recs += k;
        n -= k;
    }
    return sp->ok;
}

size_t gw_sfspill_peek(gw_sfspill_t *sp, gw_sf_rec_t *out, size_t max)
{
    size_t got = 0;
    uint32_t seg = sp->first, off = sp->read_off;
    uint32_t in_seg = sp->first_count;
    while (sp->ok && got < max && got < sp->count) {
        if (off >= in_seg) {
            /* the window goes on in the next segment (a string may straddle two) */
            if (seg == sp->last) {
                break;
            }
            seg++;
            off = 0;
            int32_t c = seg_records(sp, seg);
            if (c < 0) {
                fail(sp);
                break;
            }
            in_seg = (uint32_t)c;
            continue;
        }
        size_t k = in_seg - off;
        k = k < max - got ? k : max - got;
        if (!sp->io.read(sp->io.ctx, seg, HDR + off * REC, out + got, k * REC)) {
            fail(sp);
            break;
        }
        got += k;
        off += (uint32_t)k;
    }
    return got;
}

void gw_sfspill_consume(gw_sfspill_t *sp, size_t n)
{
    n = n < sp->count ? n : (size_t)sp->count;
    sp->count -= n;
    while (sp->nsegs > 0) {
        size_t left = sp->first_count - sp->read_off;
        size_t k = n < left ? n : left;
        sp->read_off += (uint32_t)k;
        n -= k;
        if (sp->read_off < sp->first_count || (sp->first == sp->last && sp->first_count == 0)) {
            break;   /* not finished (or the segment being written is still empty) */
        }
        /* segment fully replayed: delete it */
        sp->io.remove(sp->io.ctx, sp->first);
        sp->read_off = 0;
        if (sp->first == sp->last) {
            sp->nsegs = 0;
            sp->first_count = sp->last_count = 0;
            break;
        }
        sp->first++;
        sp->nsegs--;
        int32_t c = seg_records(sp, sp->first);
        sp->first_count = c > 0 ? (uint32_t)c : 0;
    }
}
