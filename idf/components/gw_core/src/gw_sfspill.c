#include "gw_sfspill.h"

#include <string.h>

#define HDR ((uint32_t)sizeof(gw_sfspill_hdr_t))
#define REC ((uint32_t)sizeof(gw_sf_rec_t))

/* io.size() returns an int32_t: the largest segment must fit. */
_Static_assert((uint64_t)GW_SFSPILL_MAX_KB * 1024u / GW_SFSPILL_MIN_SEGS <= INT32_MAX,
               "a spill segment must stay below 2^31 bytes");
_Static_assert(GW_SFSPILL_MIN_SEGS >= 2 && GW_SFSPILL_MIN_SEGS <= GW_SFSPILL_MAX_SEGS,
               "segment bounds");
_Static_assert(GW_SFSPILL_GC_MARKED == GW_SFSPILL_ID_LIMIT, "the queue flag is above the ids");

uint32_t gw_sfspill_plan(uint32_t max_kb, uint32_t unit, uint32_t *seg_records,
                         uint32_t *max_segs)
{
    max_kb = max_kb < GW_SFSPILL_MIN_KB   ? GW_SFSPILL_MIN_KB
             : max_kb > GW_SFSPILL_MAX_KB ? GW_SFSPILL_MAX_KB
                                          : max_kb;
    uint64_t bytes = (uint64_t)max_kb * 1024u;
    /* as many preferred-size segments as fit, within the bounds of the segment count ... */
    uint64_t segs = bytes / ((uint64_t)GW_SFSPILL_SEG_RECORDS * REC + HDR);
    segs = segs < GW_SFSPILL_MIN_SEGS   ? GW_SFSPILL_MIN_SEGS
           : segs > GW_SFSPILL_MAX_SEGS ? GW_SFSPILL_MAX_SEGS
                                        : segs;
    /* ... at most one delete step each (#122): more segments, or less of max_kb ... */
    if (unit != 0) {
        uint64_t cap = (uint64_t)unit * GW_SFSPILL_STEP_UNITS;
        if (bytes / segs > cap) {
            segs = (bytes + cap - 1) / cap;
            if (segs > GW_SFSPILL_MAX_SEGS) {
                segs = GW_SFSPILL_MAX_SEGS;
                bytes = segs * cap;
            }
        }
    }
    /* ... then each segment grows (or shrinks) to its share, header included */
    *max_segs = (uint32_t)segs;
    *seg_records = (uint32_t)((bytes / segs - HDR) / REC);
    return (uint32_t)(bytes / 1024u);
}

uint32_t gw_sfspill_fnv(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static void fail(gw_sfspill_t *sp)
{
    sp->ok = false;
}

static void tick(const gw_sfspill_t *sp)
{
    if (sp->tick != NULL) {
        sp->tick();
    }
}

enum { SEG_OTHER, SEG_DEAD, SEG_LIVE };

/* What segment @p seg is: SEG_LIVE (ours, this configuration; *recs = its records according to
 * its size), SEG_DEAD (marked dead) or SEG_OTHER (absent, foreign, torn or another
 * configuration). */
static int classify(gw_sfspill_t *sp, uint32_t seg, uint32_t *recs)
{
    int32_t size = sp->io.size(sp->io.ctx, seg);
    gw_sfspill_hdr_t h;
    if (size < (int32_t)HDR || !sp->io.read(sp->io.ctx, seg, 0, &h, sizeof(h))) {
        return SEG_OTHER;
    }
    if (h.magic == GW_SFSPILL_DEAD) {
        return SEG_DEAD;
    }
    if (h.magic != GW_SFSPILL_MAGIC || h.version != GW_SFSPILL_VERSION || h.rec_size != REC ||
        h.cfg_hash != sp->cfg_hash || h.seg_id != seg) {
        return SEG_OTHER;
    }
    *recs = ((uint32_t)size - HDR) / REC;   /* a partial last record is ignored */
    return SEG_LIVE;
}

static uint64_t seg_bytes(uint32_t records)
{
    return HDR + (uint64_t)records * REC;
}

/* The next live segment after @p seg, up to last (ids in between that are not live are
 * skipped). False if there is none. */
static bool next_live(gw_sfspill_t *sp, uint32_t seg, uint32_t *id, uint32_t *recs)
{
    while (seg != sp->last) {
        seg++;
        if (classify(sp, seg, recs) == SEG_LIVE) {
            *id = seg;
            if (seg == sp->last) {
                *recs = sp->last_count;   /* what was written, not what the size says */
            }
            return true;
        }
    }
    return false;
}

static void sort_ids(uint32_t *ids, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        uint32_t v = ids[i];
        size_t j = i;
        for (; j > 0 && ids[j - 1] > v; j--) {
            ids[j] = ids[j - 1];
        }
        ids[j] = v;
    }
}

void gw_sfspill_init(gw_sfspill_t *sp, const gw_sfspill_io_t *io, uint32_t seg_records_,
                     uint32_t max_segs, uint32_t cfg_hash, uint32_t *work, size_t work_cap)
{
    memset(sp, 0, sizeof(*sp));
    sp->io = *io;
    sp->seg_records = seg_records_ == 0 ? 1 : seg_records_;
    sp->max_segs = max_segs < 2 ? 2 : max_segs > GW_SFSPILL_MAX_SEGS ? GW_SFSPILL_MAX_SEGS
                                                                     : max_segs;
    sp->cfg_hash = cfg_hash;
    sp->max_bytes = sp->max_segs * seg_bytes(sp->seg_records);
    uint64_t step = (uint64_t)io->unit * GW_SFSPILL_STEP_UNITS;
    sp->step_bytes = step > UINT32_MAX ? UINT32_MAX : (uint32_t)step;
    sp->gc = work;
    sp->gc_cap = work != NULL ? (uint32_t)work_cap : 0;
    sp->ok = true;

    size_t n = sp->gc_cap > 0 ? io->list(io->ctx, work, sp->gc_cap) : 0;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (work[i] < GW_SFSPILL_ID_LIMIT) {   /* the others are not ours: never touched */
            work[m++] = work[i];
        }
    }
    n = m;
    sort_ids(work, n);
    sp->next_id = n > 0 ? work[n - 1] + 1 : 0;   /* never the number of a file still there */

    /* Keep the newest live segments, as many as fit in max_segs and max_bytes; everything else
     * goes to the delete queue, which grows down from the end of work (written at an index at
     * least as high as the one read). */
    size_t v = n;
    bool full = false;
    for (size_t i = n; i-- > 0;) {
        uint32_t id = work[i], r = 0;
        int kind = classify(sp, id, &r);
        if (kind == SEG_LIVE && !full) {
            uint64_t b = seg_bytes(r);
            if (sp->nsegs < sp->max_segs && sp->bytes + b <= sp->max_bytes) {
                if (sp->nsegs == 0) {
                    sp->last = id;
                    sp->last_count = r;
                }
                sp->first = id;
                sp->first_count = r;
                sp->nsegs++;
                sp->bytes += b;
                sp->count += r;
                continue;
            }
            full = true;   /* this one and every older one: over max_kb (or max_segs) */
        }
        if (kind == SEG_LIVE) {
            sp->trimmed += r;
        }
        work[--v] = id | (kind == SEG_DEAD ? GW_SFSPILL_GC_MARKED : 0);
    }
    sp->gc_head = (uint32_t)v;
    sp->gc_n = (uint32_t)(n - v);
    sp->kept_at_boot = (uint32_t)sp->count;
}

/* Queue segment @p seg for deletion (@p marked: already dead). Without room, it stays for the
 * next boot, which finds it dead (or not of this run) and deletes it. */
static void queue_delete(gw_sfspill_t *sp, uint32_t seg, bool marked)
{
    if (sp->gc_n < sp->gc_cap) {
        sp->gc[(sp->gc_head + sp->gc_n) % sp->gc_cap] = seg | (marked ? GW_SFSPILL_GC_MARKED : 0);
        sp->gc_n++;
    } else {
        sp->gc_left++;
    }
}

/* Mark segment @p seg dead in place: never replayed again, deleted by gw_sfspill_step(). */
static bool mark_dead(gw_sfspill_t *sp, uint32_t seg)
{
    static const uint32_t dead = GW_SFSPILL_DEAD;
    bool ok = sp->io.patch(sp->io.ctx, seg, 0, &dead, sizeof(dead));
    tick(sp);
    return ok;
}

/* The first segment leaves the run (replayed, or dropped for space): dead, then queued. */
static void retire_first(gw_sfspill_t *sp)
{
    uint64_t b = seg_bytes(sp->first_count);
    sp->bytes = sp->bytes > b ? sp->bytes - b : 0;
    uint32_t seg = sp->first;
    if (!mark_dead(sp, seg)) {
        fail(sp);
    }
    queue_delete(sp, seg, true);
    sp->read_off = 0;
    if (sp->nsegs <= 1 || seg == sp->last) {
        sp->nsegs = 0;
        sp->first_count = sp->last_count = 0;
        sp->bytes = 0;
        return;
    }
    sp->nsegs--;
    uint32_t id, recs;
    if (!sp->ok || !next_live(sp, seg, &id, &recs)) {
        fail(sp);   /* a segment of the run is gone */
        return;
    }
    sp->first = id;
    sp->first_count = recs;
}

static bool new_segment(gw_sfspill_t *sp)
{
    uint64_t need = seg_bytes(sp->seg_records);
    while (sp->ok && sp->nsegs > 0 &&
           (sp->nsegs >= sp->max_segs || sp->bytes + need > sp->max_bytes)) {
        /* full: the oldest segment goes, with what was not replayed of it */
        uint32_t lost = sp->first_count - sp->read_off;
        sp->dropped += lost;
        sp->count -= lost;
        retire_first(sp);
    }
    uint32_t id = 0;
    for (int tries = 0; sp->ok; tries++) {
        if (tries == 8 || sp->next_id >= GW_SFSPILL_ID_LIMIT) {
            fail(sp);
            break;
        }
        id = sp->next_id++;
        if (sp->io.size(sp->io.ctx, id) < 0) {
            break;
        }
        queue_delete(sp, id, false);   /* a file with that number that the boot did not list */
    }
    const gw_sfspill_hdr_t h = {GW_SFSPILL_MAGIC, GW_SFSPILL_VERSION, (uint16_t)REC,
                                sp->cfg_hash, id};
    if (!sp->ok || !sp->io.write(sp->io.ctx, id, 0, &h, sizeof(h))) {
        fail(sp);
        return false;
    }
    tick(sp);
    if (sp->nsegs == 0) {
        sp->first = id;
        sp->first_count = 0;
        sp->read_off = 0;
    }
    sp->last = id;
    sp->last_count = 0;
    sp->nsegs++;
    sp->bytes += HDR;
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
        /* at the record boundary: a partial record left by a power cut is overwritten */
        if (!sp->io.write(sp->io.ctx, sp->last, HDR + sp->last_count * REC, recs, k * REC)) {
            fail(sp);
            return false;
        }
        tick(sp);
        sp->last_count += (uint32_t)k;
        if (sp->first == sp->last) {
            sp->first_count = sp->last_count;
        }
        sp->count += k;
        sp->bytes += k * REC;
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
            uint32_t id, c;
            if (seg == sp->last) {
                break;
            }
            if (!next_live(sp, seg, &id, &c)) {
                fail(sp);
                break;
            }
            seg = id;
            off = 0;
            in_seg = c;
            continue;
        }
        size_t k = in_seg - off;
        k = k < max - got ? k : max - got;
        if (!sp->io.read(sp->io.ctx, seg, HDR + off * REC, out + got, k * REC)) {
            fail(sp);
            break;
        }
        tick(sp);
        got += k;
        off += (uint32_t)k;
    }
    return got;
}

void gw_sfspill_consume(gw_sfspill_t *sp, size_t n)
{
    n = n < sp->count ? n : (size_t)sp->count;
    sp->count -= n;
    while (sp->nsegs > 0 && sp->ok) {
        size_t left = sp->first_count - sp->read_off;
        size_t k = n < left ? n : left;
        sp->read_off += (uint32_t)k;
        n -= k;
        if (sp->read_off < sp->first_count || (sp->first == sp->last && sp->first_count == 0)) {
            break;   /* not finished (or the segment being written is still empty) */
        }
        retire_first(sp);   /* fully replayed */
    }
}

uint32_t gw_sfspill_gc_pending(const gw_sfspill_t *sp)
{
    return sp->gc_n + (sp->gc_busy ? 1u : 0u);
}

/* The delete in progress ends (done, or given up after an I/O error). */
static void gc_end(gw_sfspill_t *sp, bool done)
{
    sp->gc_busy = false;
    if (done) {
        sp->gc_files++;
    } else {
        sp->gc_left++;
    }
}

bool gw_sfspill_step(gw_sfspill_t *sp)
{
    if (!sp->ok || (!sp->gc_busy && sp->gc_n == 0)) {
        return false;
    }
    if (!sp->gc_busy) {
        uint32_t e = sp->gc[sp->gc_head];
        sp->gc_head = (sp->gc_head + 1) % sp->gc_cap;
        sp->gc_n--;
        sp->gc_seg = e & ~GW_SFSPILL_GC_MARKED;
        sp->gc_marked = (e & GW_SFSPILL_GC_MARKED) != 0;
        sp->gc_busy = true;
    }
    uint32_t seg = sp->gc_seg;
    int32_t size = sp->io.size(sp->io.ctx, seg);
    sp->gc_steps++;
    if (size < 0) {
        sp->gc_busy = false;   /* already gone */
        tick(sp);
        return true;
    }
    if (!sp->gc_marked) {
        /* phase 1: never replayed again, even if a power cut stops the delete here */
        sp->gc_marked = true;
        if (size >= (int32_t)sizeof(uint32_t)) {
            if (!mark_dead(sp, seg)) {
                gc_end(sp, false);
            }
            return true;
        }
    }
    /* phase 2: one step of at most step_bytes; FAT frees whole clusters from the end */
    bool ok;
    if (sp->step_bytes != 0 && (uint32_t)size > sp->step_bytes) {
        uint32_t to = ((uint32_t)size - 1u) / sp->step_bytes * sp->step_bytes;
        ok = sp->io.truncate(sp->io.ctx, seg, to);
        if (!ok) {
            gc_end(sp, false);
        }
    } else {
        ok = sp->io.remove(sp->io.ctx, seg);
        gc_end(sp, ok);
    }
    tick(sp);
    return true;
}
