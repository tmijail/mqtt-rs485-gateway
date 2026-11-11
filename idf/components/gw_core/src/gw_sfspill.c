#include "gw_sfspill.h"

#include <string.h>

#define REC   ((uint32_t)sizeof(gw_sf_rec_t))
#define BHDR  ((uint32_t)sizeof(gw_sfspill_blk_t))
#define BLOCK GW_SFSPILL_BLOCK

_Static_assert(sizeof(gw_sf_rec_t) == 24, "the record size is part of the card format");
_Static_assert(sizeof(gw_sfspill_blk_t) == 32, "the block header is 32 bytes (#124)");
_Static_assert(BHDR + GW_SFSPILL_BLK_RECS * 24u <= GW_SFSPILL_BLOCK, "records of a block");
_Static_assert(BHDR + (GW_SFSPILL_BLK_RECS + 1u) * 24u > GW_SFSPILL_BLOCK, "a full block");
_Static_assert(sizeof(gw_sfspill_sb_t) <= GW_SFSPILL_SB_BYTES, "the superblock is one sector");
_Static_assert(sizeof(gw_sfspill_sb_t) == offsetof(gw_sfspill_sb_t, crc) + 4,
               "no implicit padding in the superblock image");
_Static_assert(GW_SFSPILL_MAX_EXTENTS % 8 == 0, "extent bitmap");
_Static_assert((uint64_t)GW_SFSPILL_MAX_EXTENTS * GW_SFSPILL_EXT_MAX >= (uint64_t)GW_SFSPILL_MAX_KB * 1024u,
               "4 MiB extents hold the largest max_kb");

/* ---------------------------------------------------------------- helpers */

uint32_t gw_sfspill_fnv(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* CRC-32C (Castagnoli, reflected 0x82F63B78), four bits at a time: 64 bytes of table. */
static const uint32_t s_crc_nib[16] = {
    0x00000000u, 0x105EC76Fu, 0x20BD8EDEu, 0x30E349B1u, 0x417B1DBCu, 0x5125DAD3u,
    0x61C69362u, 0x7198540Du, 0x82F63B78u, 0x92A8FC17u, 0xA24BB5A6u, 0xB21572C9u,
    0xC38D26C4u, 0xD3D3E1ABu, 0xE330A81Au, 0xF36E6F75u,
};

uint32_t gw_sfspill_crc32c(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ s_crc_nib[crc & 15u];
        crc = (crc >> 4) ^ s_crc_nib[crc & 15u];
    }
    return ~crc;
}

uint32_t gw_sfspill_plan(uint32_t max_kb, uint32_t unit, uint32_t *ext_blocks, uint32_t *extents)
{
    max_kb = max_kb < GW_SFSPILL_MIN_KB   ? GW_SFSPILL_MIN_KB
             : max_kb > GW_SFSPILL_MAX_KB ? GW_SFSPILL_MAX_KB
                                          : max_kb;
    uint64_t bytes = (uint64_t)max_kb * 1024u;
    uint64_t ext = GW_SFSPILL_EXT_MAX;
    if (unit != 0 && (uint64_t)unit * GW_SFSPILL_STEP_UNITS < ext) {
        ext = (uint64_t)unit * GW_SFSPILL_STEP_UNITS;   /* one FAT step allocates or frees it */
    }
    ext = ext / BLOCK * BLOCK;
    if (ext < BLOCK) {
        ext = BLOCK;
    }
    if (ext > bytes) {
        ext = bytes / BLOCK * BLOCK;   /* a small max_kb: one extent of that size */
    }
    uint64_t e = bytes / ext;
    if (e > GW_SFSPILL_MAX_EXTENTS) {
        e = GW_SFSPILL_MAX_EXTENTS;
    }
    *ext_blocks = (uint32_t)(ext / BLOCK);
    *extents = (uint32_t)e;
    return (uint32_t)(e * ext / 1024u);
}

#define MEM_WBUF 0u
#define MEM_RBUF BLOCK
#define MEM_GC   (2u * BLOCK)
#define MEM_SIZE (MEM_GC + GW_SFSPILL_GC_MAX * 4u)

size_t gw_sfspill_mem_size(void)
{
    return MEM_SIZE;
}

static void set_why(gw_sfspill_t *sp, const char *why)
{
    size_t n = strlen(why);
    n = n < sizeof(sp->why) - 1 ? n : sizeof(sp->why) - 1;
    memcpy(sp->why, why, n);
    sp->why[n] = '\0';
}

/* The spill stops: what waits on the card cannot be replayed any more (#134: it is counted
 * apart, and nothing waits for it). */
static void fail(gw_sfspill_t *sp, const char *why)
{
    if (!sp->ok) {
        return;
    }
    sp->ok = false;
    set_why(sp, why);
    sp->stranded = sp->ring_count;
    sp->ring_count = 0;
    sp->peeked = false;
}

void gw_sfspill_stop(gw_sfspill_t *sp, const char *why)
{
    fail(sp, why);
}

const char *gw_sfspill_found_text(const gw_sfspill_t *sp)
{
    switch (sp->found) {
    case GW_SFSPILL_FOUND_NONE:
        return "new ring: no valid ring on the card";
    case GW_SFSPILL_FOUND_OTHER:
        return "new ring: another configuration or geometry";
    case GW_SFSPILL_FOUND_SUPER:
        return "ring found with its superblock";
    case GW_SFSPILL_FOUND_VOTE:
        return "ring found again without a superblock (vote)";
    default:
        return "boot recovery not ended";
    }
}

static void tick(const gw_sfspill_t *sp)
{
    if (sp->tick != NULL) {
        sp->tick();
    }
}

void gw_sfspill_init(gw_sfspill_t *sp, const gw_sfspill_io_t *io, uint32_t ext_blocks,
                     uint32_t extents, uint32_t cfg_hash, void *mem)
{
    memset(sp, 0, sizeof(*sp));
    sp->io = *io;
    sp->cfg_hash = cfg_hash;
    sp->ext_blocks = ext_blocks == 0 ? 1 : ext_blocks;
    sp->extents = extents == 0 ? 1 : extents > GW_SFSPILL_MAX_EXTENTS ? GW_SFSPILL_MAX_EXTENTS
                                                                     : extents;
    sp->nblocks = sp->ext_blocks * sp->extents;
    sp->phase = GW_SFSPILL_BOOT_SUPER;
    sp->ok = true;
    if (mem == NULL) {
        fail(sp, "no memory");
        return;
    }
    uint8_t *m = mem;
    sp->wbuf = m + MEM_WBUF;
    sp->rbuf = m + MEM_RBUF;
    sp->gc = (uint32_t *)(void *)(m + MEM_GC);
}

static uint32_t f_extent(const gw_sfspill_t *sp, uint64_t seq)
{
    return GW_SFSPILL_F_EXTENT | (uint32_t)(seq % sp->nblocks / sp->ext_blocks);
}

static uint32_t off_in_extent(const gw_sfspill_t *sp, uint64_t seq)
{
    return (uint32_t)(seq % sp->nblocks % sp->ext_blocks) * BLOCK;
}

static bool ext_ok(const gw_sfspill_t *sp, uint32_t e)
{
    return (sp->ext_ok[e / 8] >> (e % 8)) & 1u;
}

/* ---------------------------------------------------------------- deletes */

static void gc_push(gw_sfspill_t *sp, uint32_t f)
{
    if (sp->gc_n < GW_SFSPILL_GC_MAX) {
        sp->gc[(sp->gc_head + sp->gc_n) % GW_SFSPILL_GC_MAX] = f;
        sp->gc_n++;
    } else {
        sp->gc_left++;   /* the next boot finds it (not of this ring) */
    }
}

uint32_t gw_sfspill_gc_pending(const gw_sfspill_t *sp)
{
    return sp->gc_n + (sp->gc_busy ? 1u : 0u);
}

static void gc_end(gw_sfspill_t *sp, bool done)
{
    sp->gc_busy = false;
    if (done) {
        sp->gc_files++;
    } else {
        sp->gc_left++;
    }
}

/* One delete step (#122): a truncate of at most 128 allocation units, or the unlink of at most
 * that. Nothing reads a file in the delete queue, so a cut delete needs no mark (#151). */
static bool gc_step(gw_sfspill_t *sp)
{
    if (!sp->gc_busy && sp->gc_n == 0) {
        return false;
    }
    if (!sp->gc_busy) {
        sp->gc_f = sp->gc[sp->gc_head];
        sp->gc_head = (sp->gc_head + 1) % GW_SFSPILL_GC_MAX;
        sp->gc_n--;
        sp->gc_busy = true;
    }
    uint32_t f = sp->gc_f;
    int32_t size = sp->io.size(sp->io.ctx, f);
    sp->gc_steps++;
    if (size < 0) {
        sp->gc_busy = false;   /* already gone */
        tick(sp);
        return true;
    }
    uint64_t step = (uint64_t)sp->io.unit * GW_SFSPILL_STEP_UNITS;
    if (step != 0 && step < (uint64_t)size) {
        uint32_t to = (uint32_t)(((uint64_t)size - 1u) / step * step);
        if (!sp->io.truncate(sp->io.ctx, f, to)) {
            gc_end(sp, false);
        }
    } else {
        gc_end(sp, sp->io.remove(sp->io.ctx, f));
    }
    tick(sp);
    return true;
}

/* ---------------------------------------------------------------- blocks and superblocks */

/* A block's own checks: magic, version, record size, configuration, count and CRC. */
static bool blk_sound(const gw_sfspill_t *sp, const uint8_t *buf)
{
    gw_sfspill_blk_t h;
    memcpy(&h, buf, sizeof(h));
    if (h.magic != GW_SFSPILL_BLK_MAGIC || h.version != GW_SFSPILL_VERSION2 || h.rec_size != REC ||
        h.cfg_hash != sp->cfg_hash || h.nrec == 0 || h.nrec > GW_SFSPILL_BLK_RECS ||
        h.flags != 0) {
        return false;
    }
    uint32_t want = h.crc;
    h.crc = 0;
    uint32_t crc = gw_sfspill_crc32c(0, &h, sizeof(h));
    crc = gw_sfspill_crc32c(crc, buf + BHDR, (size_t)h.nrec * REC);
    return crc == want;
}

static uint16_t blk_nrec(const uint8_t *buf)
{
    gw_sfspill_blk_t h;
    memcpy(&h, buf, sizeof(h));
    return h.nrec;
}

/* Read block @p seq into @p buf: 1 valid (this ring, this position, this seq), 0 not, -1 I/O
 * error (the spill failed). */
static int read_block(gw_sfspill_t *sp, uint64_t seq, uint8_t *buf)
{
    int r = sp->io.read(sp->io.ctx, f_extent(sp, seq), off_in_extent(sp, seq), buf, BLOCK);
    tick(sp);
    if (r < 0) {
        fail(sp, "card read failed");
        return -1;
    }
    if (r == 0 || !blk_sound(sp, buf)) {
        return 0;
    }
    gw_sfspill_blk_t h;
    memcpy(&h, buf, sizeof(h));
    return h.nonce == sp->nonce && h.seq == seq ? 1 : 0;
}

static uint32_t sb_crc(const gw_sfspill_sb_t *s)
{
    return gw_sfspill_crc32c(0, s, offsetof(gw_sfspill_sb_t, crc));
}

static bool sb_sound(const gw_sfspill_sb_t *s)
{
    return s->magic == GW_SFSPILL_SB_MAGIC && s->version == GW_SFSPILL_VERSION2 &&
           s->rec_size == REC && s->crc == sb_crc(s);
}

/* The superblock describes the ring this boot plans: same configuration and geometry. */
static bool sb_fits(const gw_sfspill_t *sp, const gw_sfspill_sb_t *s)
{
    return s->cfg_hash == sp->cfg_hash && s->ext_blocks == sp->ext_blocks &&
           s->extents == sp->extents && s->nblocks > 0 && s->nblocks % s->ext_blocks == 0 &&
           s->nblocks <= sp->ext_blocks * sp->extents && s->head_hint >= s->tail_seq &&
           s->head_hint - s->tail_seq <= s->nblocks && s->tail_rec <= GW_SFSPILL_BLK_RECS;
}

static bool sb_gap_ok(const gw_sfspill_t *sp, uint32_t now_ms)
{
    return !sp->sb_written || (uint32_t)(now_ms - sp->sb_last_ms) >= GW_SFSPILL_SB_GAP_MS;
}

static bool empty(const gw_sfspill_t *sp)
{
    return sp->tail >= sp->head;
}

/* The replay position moved since the last superblock: due once 64 KiB were replayed, or once
 * everything was (so that a reboot does not send it again). */
static bool ckpt_due(const gw_sfspill_t *sp)
{
    const gw_sfspill_sb_t *last = &sp->sb[sp->sb_next ^ 1u];   /* the copy written last */
    bool moved = sp->ck_pending > 0 || sp->tail != last->tail_seq ||
                 sp->tail_rec != last->tail_rec;
    return sp->ck_pending > GW_SFSPILL_CKPT_RECS - GW_SF_SAMPLE_MAX_RECS ||
           (empty(sp) && moved && sp->sb_written);
}

/* The head hint goes to the card every H blocks, H at most a quarter of the ring: the hint
 * of either copy is then never a full lap behind the head. */
static bool hint_due(const gw_sfspill_t *sp)
{
    uint32_t h = sp->nblocks / 4 < GW_SFSPILL_HINT_BLOCKS ? sp->nblocks / 4 : GW_SFSPILL_HINT_BLOCKS;
    return sp->head - sp->hint_at >= (h > 0 ? h : 1);
}

/* Write the superblock copy with the lower generation: one sector. The files of both copies
 * are made first, one per step, so that a full card never stops a later superblock. */
static void sb_write(gw_sfspill_t *sp, uint32_t now_ms)
{
    for (uint32_t i = 0; i < 2; i++) {
        if (!(sp->sb_alloc & (1u << i))) {
            int r = sp->io.alloc(sp->io.ctx, GW_SFSPILL_F_SUPER | i, GW_SFSPILL_SB_BYTES);
            tick(sp);
            if (r <= 0) {
                fail(sp, r == 0 ? "card full" : "superblock file failed");
                return;
            }
            sp->sb_alloc |= (uint8_t)(1u << i);
            return;   /* the write is the next step */
        }
    }
    uint8_t c = sp->sb_next;
    uint32_t f = GW_SFSPILL_F_SUPER | c;
    gw_sfspill_sb_t s = {
        .magic = GW_SFSPILL_SB_MAGIC, .version = GW_SFSPILL_VERSION2, .rec_size = (uint16_t)REC,
        .gen = sp->gen + 1, .nonce = sp->nonce, .cfg_hash = sp->cfg_hash,
        .ext_blocks = sp->ext_blocks, .extents = sp->extents, .nblocks = sp->nblocks,
        .tail_seq = sp->tail, .head_hint = sp->head, .tail_rec = sp->tail_rec,
        .count = sp->ring_count > UINT32_MAX ? UINT32_MAX : (uint32_t)sp->ring_count,
    };
    s.crc = sb_crc(&s);
    memset(sp->wbuf, 0, GW_SFSPILL_SB_BYTES);
    memcpy(sp->wbuf, &s, sizeof(s));
    bool ok = sp->io.write(sp->io.ctx, f, 0, sp->wbuf, GW_SFSPILL_SB_BYTES);
    tick(sp);
    if (!ok) {
        fail(sp, "superblock write failed");
        return;
    }
    sp->sb[c] = s;   /* what the card has now (ckpt_due() compares with it) */
    sp->sb_valid[c] = true;
    sp->gen++;
    sp->sb_next = c ^ 1u;
    sp->sb_due = false;
    sp->sb_written = true;
    sp->sb_last_ms = now_ms;
    if (sp->ck_pending > 0) {
        sp->checkpoints++;
    }
    sp->ck_pending = 0;
    sp->hint_at = sp->head;
    sp->sb_writes++;
}

/* A new ring instance: a new nonce invalidates every block on the card at once. */
static void new_ring(gw_sfspill_t *sp, uint32_t old_nonce)
{
    uint32_t n = sp->random != NULL ? sp->random() : old_nonce + 0x9E3779B9u;
    while (n == old_nonce || n == 0) {
        n++;
    }
    sp->nonce = n;
    sp->nblocks = sp->ext_blocks * sp->extents;
    sp->head = sp->tail = sp->hint_at = 0;
    sp->tail_rec = 0;
    sp->ring_count = 0;
    sp->rvalid = false;
    sp->sb_due = true;
    memset(sp->ext_ok, 0, sizeof(sp->ext_ok));
}

/* ---------------------------------------------------------------- boot recovery */

static void boot_super(gw_sfspill_t *sp)
{
    for (uint32_t c = 0; c < 2; c++) {
        memset(&sp->sb[c], 0, sizeof(sp->sb[c]));
        int r = sp->io.read(sp->io.ctx, GW_SFSPILL_F_SUPER | c, 0, &sp->sb[c], sizeof(sp->sb[c]));
        tick(sp);
        if (r < 0) {
            fail(sp, "superblock read failed");
            return;
        }
        sp->sb_valid[c] = r > 0 && sb_sound(&sp->sb[c]);
        if (!sp->sb_valid[c]) {
            memset(&sp->sb[c], 0, sizeof(sp->sb[c]));
        }
        sp->scan_reads++;
    }
    int best = !sp->sb_valid[0] && !sp->sb_valid[1] ? -1
               : !sp->sb_valid[1]                    ? 0
               : !sp->sb_valid[0]                    ? 1
               : sp->sb[1].gen > sp->sb[0].gen       ? 1
                                                     : 0;
    sp->phase = GW_SFSPILL_BOOT_LIST;
    if (best < 0) {
        sp->gen = 0;
        sp->sb_next = 0;
        return;   /* no superblock: the probe decides after the listing */
    }
    const gw_sfspill_sb_t *s = &sp->sb[best];
    sp->gen = s->gen;
    sp->sb_next = (uint8_t)(best ^ 1);
    if (!sb_fits(sp, s)) {
        new_ring(sp, s->nonce);   /* another configuration or geometry */
        sp->found = GW_SFSPILL_FOUND_OTHER;
        return;
    }
    sp->found = GW_SFSPILL_FOUND_SUPER;
    sp->nonce = s->nonce;
    sp->nblocks = s->nblocks;
    sp->tail = s->tail_seq;
    sp->tail_rec = s->tail_rec;
    sp->head = s->head_hint;
    sp->hint_at = s->head_hint;
    sp->ring_count = s->count;
    sp->scan_seq = s->head_hint;
    sp->scan_end = s->head_hint + sp->nblocks;
    sp->sb_written = true;   /* for the gap: a copy may have been written just before the boot */
    sp->sb_last_ms = sp->boot_start_ms;
}

static void list_each(void *arg, uint32_t f)
{
    gw_sfspill_t *sp = arg;
    uint32_t kind = f & GW_SFSPILL_F_KIND;
    if (kind == GW_SFSPILL_F_SUPER) {
        return;
    }
    if (kind == GW_SFSPILL_F_EXTENT) {
        uint32_t e = f & ~GW_SFSPILL_F_KIND;
        if (e < sp->nblocks / sp->ext_blocks) {
            sp->probe_e++;   /* counted here; reset before the probe */
        } else {
            gc_push(sp, f);   /* beyond the ring */
        }
        return;
    }
    if (f < GW_SFSPILL_ID_LIMIT) {
        gc_push(sp, f);   /* #151: a segment of the step 1 format: deleted, never replayed */
    }
}

/* The listing, then the probe (no superblock but extents), the scan, or ready. */
static void boot_list(gw_sfspill_t *sp, uint32_t now_ms)
{
    sp->probe_e = 0;
    bool ok = sp->io.list(sp->io.ctx, list_each, sp);
    tick(sp);
    if (!ok) {
        fail(sp, "card directory unreadable");
        return;
    }
    sp->scan_run = 0;
    if (sp->sb_valid[0] || sp->sb_valid[1]) {
        sp->phase = sp->sb_due ? GW_SFSPILL_READY : GW_SFSPILL_BOOT_SCAN;
    } else if (sp->probe_e > 0) {
        sp->probe_e = 0;
        sp->phase = GW_SFSPILL_BOOT_PROBE;
    } else {
        new_ring(sp, 0);
        sp->found = GW_SFSPILL_FOUND_NONE;
        sp->phase = GW_SFSPILL_READY;
    }
    if (sp->phase == GW_SFSPILL_READY) {
        sp->boot_ms = now_ms - sp->boot_start_ms;
    }
}

/* No valid superblock: the first block of each extent votes for its ring (nonce). */
static void boot_probe(gw_sfspill_t *sp, uint32_t now_ms)
{
    for (int k = 0; k < GW_SFSPILL_SCAN_READS && sp->probe_e < sp->extents; k++) {
        uint32_t e = sp->probe_e++;
        int r = sp->io.read(sp->io.ctx, GW_SFSPILL_F_EXTENT | e, 0, sp->rbuf, BLOCK);
        tick(sp);
        sp->scan_reads++;
        if (r < 0) {
            fail(sp, "card read failed");
            return;
        }
        gw_sfspill_blk_t h;
        memcpy(&h, sp->rbuf, sizeof(h));
        if (r == 0 || !blk_sound(sp, sp->rbuf) ||
            h.seq % sp->nblocks != (uint64_t)e * sp->ext_blocks) {
            continue;
        }
        int slot = -1;
        for (int i = 0; i < 4; i++) {
            if (sp->vote_n[i] > 0 && sp->vote_nonce[i] == h.nonce) {
                slot = i;
                break;
            }
            if (slot < 0 && sp->vote_n[i] == 0) {
                slot = i;
            }
        }
        if (slot < 0) {
            continue;   /* more than four rings on one card: the rest are ignored */
        }
        if (sp->vote_n[slot] == 0 || h.seq > sp->vote_seq[slot]) {
            sp->vote_seq[slot] = h.seq;
        }
        sp->vote_nonce[slot] = h.nonce;
        sp->vote_n[slot]++;
    }
    if (sp->probe_e < sp->extents) {
        return;
    }
    int best = -1;
    for (int i = 0; i < 4; i++) {
        if (sp->vote_n[i] > 0 &&
            (best < 0 || sp->vote_n[i] > sp->vote_n[best] ||
             (sp->vote_n[i] == sp->vote_n[best] && sp->vote_seq[i] > sp->vote_seq[best]))) {
            best = i;
        }
    }
    if (best < 0) {
        new_ring(sp, 0);
        sp->found = GW_SFSPILL_FOUND_NONE;
        sp->phase = GW_SFSPILL_READY;
        sp->boot_ms = now_ms - sp->boot_start_ms;
        return;
    }
    /* the replay position is lost with the superblocks: everything still on the card is sent
     * (duplicates, never garbage); a new superblock is written before the next block */
    sp->found = GW_SFSPILL_FOUND_VOTE;
    sp->nonce = sp->vote_nonce[best];
    sp->head = sp->vote_seq[best] + 1;
    sp->tail = 0;
    sp->tail_rec = 0;
    sp->ring_count = 0;
    sp->scan_seq = sp->head;
    sp->scan_end = sp->head + sp->nblocks;
    sp->sb_due = true;
    sp->phase = GW_SFSPILL_BOOT_SCAN;
}

static void finish_scan(gw_sfspill_t *sp, uint32_t now_ms)
{
    uint64_t start = sp->scan_end - sp->nblocks;   /* where the scan began */
    uint64_t floor = sp->head > sp->nblocks ? sp->head - sp->nblocks : 0;
    if (sp->tail < floor) {
        /* overwritten since the superblock: the records of those blocks are not known, the
         * count becomes an estimate (until the replay empties the ring) */
        uint64_t est = (floor - sp->tail) * GW_SFSPILL_BLK_RECS;
        est = est > sp->tail_rec ? est - sp->tail_rec : 0;
        sp->ring_count = sp->ring_count > est ? sp->ring_count - est : 0;
        sp->tail = floor;
        sp->tail_rec = 0;
    }
    if (sp->sb_due) {
        /* after the probe nothing is known before the scan: full blocks are assumed */
        sp->ring_count = (start > sp->tail ? start - sp->tail : 0) * GW_SFSPILL_BLK_RECS;
    }
    sp->ring_count += sp->scan_recs;
    uint64_t live = sp->head - sp->tail;
    uint64_t lo = live > 0 ? 1 : 0, hi = live * GW_SFSPILL_BLK_RECS;
    sp->ring_count = sp->ring_count < lo ? lo : sp->ring_count > hi ? hi : sp->ring_count;
    sp->kept_at_boot = (uint32_t)sp->ring_count;
    sp->rvalid = false;
    sp->phase = GW_SFSPILL_READY;
    sp->boot_ms = now_ms - sp->boot_start_ms;
}

/* The head: forward from the hint while the blocks follow; at most GW_SFSPILL_HOLES_MAX
 * invalid blocks in a row are skipped (holes, met again and counted by the replay). A block of
 * this ring from a later lap at the expected position (the hint is older than the card: a lost
 * superblock write) moves the scan forward to it. */
static void boot_scan(gw_sfspill_t *sp, uint32_t now_ms)
{
    for (int k = 0; k < GW_SFSPILL_SCAN_READS; k++) {
        if (sp->scan_seq >= sp->scan_end || sp->scan_run > GW_SFSPILL_HOLES_MAX) {
            finish_scan(sp, now_ms);
            return;
        }
        int r = read_block(sp, sp->scan_seq, sp->rbuf);
        sp->scan_reads++;
        if (r < 0) {
            return;
        }
        gw_sfspill_blk_t h;
        memcpy(&h, sp->rbuf, sizeof(h));
        if (r == 0 && h.nonce == sp->nonce && h.seq > sp->scan_seq &&
            (h.seq - sp->scan_seq) % sp->nblocks == 0 && blk_sound(sp, sp->rbuf)) {
            sp->scan_seq = h.seq;   /* a later lap */
            sp->scan_end = h.seq + sp->nblocks;
            r = 1;
        }
        if (r > 0) {
            sp->scan_recs += blk_nrec(sp->rbuf);
            sp->head = sp->scan_seq + 1;
            sp->scan_run = 0;
        } else {
            sp->scan_run++;
        }
        sp->scan_seq++;
    }
}

/* ---------------------------------------------------------------- normal operation */

/* The next block's extent is not known to be whole: allocate it (one bounded step). On a full
 * card during the first lap the ring stops at the extents it has: every block written so far
 * has seq < its new size, so it stays valid. */
static void alloc_step(gw_sfspill_t *sp, uint32_t e)
{
    int r = sp->io.alloc(sp->io.ctx, GW_SFSPILL_F_EXTENT | e, sp->ext_blocks * BLOCK);
    tick(sp);
    sp->allocs++;
    if (r < 0) {
        fail(sp, "extent allocation failed");
        return;
    }
    if (r > 0) {
        sp->ext_ok[e / 8] |= (uint8_t)(1u << (e % 8));
        return;
    }
    uint64_t start = (uint64_t)e * sp->ext_blocks;
    if (e > 0 && sp->head < sp->nblocks && sp->head == start) {
        sp->nblocks = e * sp->ext_blocks;
        sp->sb_due = true;   /* the new size is on the card before the ring wraps */
        gc_push(sp, GW_SFSPILL_F_EXTENT | e);
        return;
    }
    fail(sp, "card full");
}

int gw_sfspill_prepare(gw_sfspill_t *sp, bool want_block, uint32_t now_ms)
{
    if (!sp->ok) {
        return GW_SFSPILL_IDLE;
    }
    switch (sp->phase) {
    case GW_SFSPILL_BOOT_SUPER:
        sp->boot_start_ms = now_ms;
        boot_super(sp);
        return GW_SFSPILL_DID;
    case GW_SFSPILL_BOOT_LIST:
        boot_list(sp, now_ms);
        return GW_SFSPILL_DID;
    case GW_SFSPILL_BOOT_PROBE:
        boot_probe(sp, now_ms);
        return GW_SFSPILL_DID;
    case GW_SFSPILL_BOOT_SCAN:
        boot_scan(sp, now_ms);
        return GW_SFSPILL_DID;
    default:
        break;
    }
    /* #142: the head hint also goes before a block (as in gw_sfspill_flush_step()): when each
     * pass wants a block, a hint after the blocks would never come */
    if ((sp->sb_due || ckpt_due(sp) || hint_due(sp)) && sb_gap_ok(sp, now_ms)) {
        sb_write(sp, now_ms);
        return GW_SFSPILL_DID;
    }
    if (want_block && !sp->sb_due) {
        uint32_t e = (uint32_t)(sp->head % sp->nblocks / sp->ext_blocks);
        if (!ext_ok(sp, e)) {
            alloc_step(sp, e);
            return GW_SFSPILL_DID;
        }
        return GW_SFSPILL_BLOCK_READY;
    }
    return gc_step(sp) ? GW_SFSPILL_DID : GW_SFSPILL_IDLE;
}

/* The replay position moved since the last superblock write. */
static bool pos_moved(const gw_sfspill_t *sp)
{
    const gw_sfspill_sb_t *last = &sp->sb[sp->sb_next ^ 1u];
    return sp->ck_pending != 0 || sp->tail != last->tail_seq || sp->tail_rec != last->tail_rec;
}

bool gw_sfspill_checkpoint(gw_sfspill_t *sp, uint32_t now_ms)
{
    if (!sp->ok || sp->phase != GW_SFSPILL_READY || !sb_gap_ok(sp, now_ms) || sp->sb_due) {
        return false;
    }
    if (!pos_moved(sp)) {
        return false;
    }
    uint32_t n = sp->sb_writes;
    for (int i = 0; i < 3 && sp->ok && sp->sb_writes == n; i++) {
        sb_write(sp, now_ms);   /* the superblock files first, if this boot has not made them */
    }
    return sp->sb_writes != n;
}

int gw_sfspill_flush_step(gw_sfspill_t *sp, bool want_block, uint32_t now_ms)
{
    if (!sp->ok || sp->phase != GW_SFSPILL_READY) {
        return GW_SFSPILL_IDLE;
    }
    bool gap = sb_gap_ok(sp, now_ms);
    if (sp->sb_due) {
        if (!gap) {
            return GW_SFSPILL_WAIT;   /* a new geometry or nonce goes first */
        }
        sb_write(sp, now_ms);
        return GW_SFSPILL_DID;
    }
    if (want_block) {
        /* the head hint as in normal operation: a short boot scan, and never a lap behind */
        if (hint_due(sp) && gap) {
            sb_write(sp, now_ms);
            return GW_SFSPILL_DID;
        }
        uint32_t e = (uint32_t)(sp->head % sp->nblocks / sp->ext_blocks);
        if (!ext_ok(sp, e)) {
            alloc_step(sp, e);
            return GW_SFSPILL_DID;
        }
        return GW_SFSPILL_BLOCK_READY;
    }
    /* the end: the head and the replay position on the card, in one superblock */
    if (sp->head != sp->hint_at || pos_moved(sp)) {
        if (!gap) {
            return GW_SFSPILL_WAIT;
        }
        sb_write(sp, now_ms);
        return GW_SFSPILL_DID;
    }
    return GW_SFSPILL_IDLE;
}

bool gw_sfspill_busy(const gw_sfspill_t *sp)
{
    return sp->ok && (sp->phase != GW_SFSPILL_READY || sp->sb_due || ckpt_due(sp) ||
                      hint_due(sp) || gw_sfspill_gc_pending(sp) > 0);
}

gw_sf_rec_t *gw_sfspill_block_recs(gw_sfspill_t *sp)
{
    return (gw_sf_rec_t *)(void *)(sp->wbuf + BHDR);
}

/* The ring is full: the block about to be overwritten is the oldest one waiting. Its records
 * are counted as dropped (the newest data is kept). */
static void drop_tail(gw_sfspill_t *sp)
{
    uint32_t nrec = 0;
    if (sp->rvalid && sp->rseq == sp->tail) {
        nrec = blk_nrec(sp->rbuf);
    } else {
        gw_sfspill_blk_t h;
        int r = sp->io.read(sp->io.ctx, f_extent(sp, sp->tail), off_in_extent(sp, sp->tail), &h,
                            sizeof(h));
        tick(sp);
        if (r < 0) {
            fail(sp, "card read failed");
            return;
        }
        if (r > 0 && h.magic == GW_SFSPILL_BLK_MAGIC && h.nonce == sp->nonce &&
            h.seq == sp->tail && h.nrec <= GW_SFSPILL_BLK_RECS) {
            nrec = h.nrec;
        } else {
            sp->holes++;
        }
    }
    uint32_t lost = nrec > sp->tail_rec ? nrec - sp->tail_rec : 0;
    sp->dropped += lost;
    sp->ring_count = sp->ring_count > lost ? sp->ring_count - lost : 0;
    if (sp->rvalid && sp->rseq == sp->tail) {
        sp->rvalid = false;
    }
    sp->tail++;
    sp->tail_rec = 0;
}

bool gw_sfspill_put_block(gw_sfspill_t *sp, size_t n)
{
    if (!sp->ok || sp->phase != GW_SFSPILL_READY || sp->sb_due || n == 0 ||
        n > GW_SFSPILL_BLK_RECS) {
        return false;
    }
    uint64_t seq = sp->head;
    if (seq - sp->tail >= sp->nblocks) {
        drop_tail(sp);
        if (!sp->ok) {
            return false;
        }
    }
    gw_sfspill_blk_t h = {
        .magic = GW_SFSPILL_BLK_MAGIC, .version = GW_SFSPILL_VERSION2, .rec_size = (uint16_t)REC,
        .nonce = sp->nonce, .cfg_hash = sp->cfg_hash, .seq = seq, .nrec = (uint16_t)n,
    };
    uint32_t crc = gw_sfspill_crc32c(0, &h, sizeof(h));
    h.crc = gw_sfspill_crc32c(crc, sp->wbuf + BHDR, n * REC);
    memcpy(sp->wbuf, &h, sizeof(h));
    memset(sp->wbuf + BHDR + n * REC, 0, BLOCK - BHDR - n * REC);
    bool ok = sp->io.write(sp->io.ctx, f_extent(sp, seq), off_in_extent(sp, seq), sp->wbuf, BLOCK);
    tick(sp);
    if (!ok) {
        fail(sp, "block write failed");
        return false;
    }
    if (sp->rvalid && sp->rseq % sp->nblocks == seq % sp->nblocks) {
        sp->rvalid = false;
    }
    sp->head++;
    sp->blocks++;
    sp->written += n;
    sp->ring_count += n;
    return true;
}

/* ---------------------------------------------------------------- replay */

uint64_t gw_sfspill_waiting(const gw_sfspill_t *sp)
{
    if (!sp->ok) {
        return 0;
    }
    return sp->ring_count > 0 || sp->tail >= sp->head ? sp->ring_count : 1;
}

bool gw_sfspill_holds(const gw_sfspill_t *sp)
{
    return sp->ok && (sp->phase != GW_SFSPILL_READY || !empty(sp));
}

size_t gw_sfspill_peek(gw_sfspill_t *sp, gw_sf_rec_t *out, size_t max)
{
    sp->peeked = false;
    if (!sp->ok || sp->phase != GW_SFSPILL_READY || sp->ck_pending >= GW_SFSPILL_CKPT_RECS) {
        return 0;
    }
    /* the duplicates bound: at most GW_SFSPILL_CKPT_RECS replayed between two superblocks;
     * with less room than the longest sample, the checkpoint comes first (a sample is never
     * cut by the window) */
    uint32_t room = GW_SFSPILL_CKPT_RECS - sp->ck_pending;
    if (room < GW_SF_SAMPLE_MAX_RECS || max == 0) {
        return 0;
    }
    max = max < room ? max : room;
    for (int reads = 0; sp->tail < sp->head;) {
        if (!sp->rvalid || sp->rseq != sp->tail) {
            if (reads++ == GW_SFSPILL_SCAN_READS) {
                return 0;   /* the next call goes on (bounded work in one pass) */
            }
            int r = read_block(sp, sp->tail, sp->rbuf);
            if (r < 0) {
                return 0;
            }
            if (r == 0) {
                sp->holes++;   /* lost (a power cut, a damaged card): never replayed */
                sp->tail++;
                sp->tail_rec = 0;
                continue;
            }
            sp->rvalid = true;
            sp->rseq = sp->tail;
        }
        uint32_t nrec = blk_nrec(sp->rbuf);
        if (sp->tail_rec >= nrec) {
            sp->tail++;
            sp->tail_rec = 0;
            continue;
        }
        size_t k = nrec - sp->tail_rec;
        k = k < max ? k : max;
        memcpy(out, sp->rbuf + BHDR + (size_t)sp->tail_rec * REC, k * REC);
        sp->peeked = true;
        return k;
    }
    sp->ring_count = 0;   /* empty: an estimate from the boot ends here */
    return 0;
}

void gw_sfspill_consume(gw_sfspill_t *sp, size_t n)
{
    if (!sp->ok || n == 0) {
        sp->peeked = false;
        return;
    }
    if (sp->peeked && sp->rvalid && sp->rseq == sp->tail) {
        uint32_t nrec = blk_nrec(sp->rbuf);
        uint32_t k = nrec - sp->tail_rec;
        k = n < k ? (uint32_t)n : k;
        sp->tail_rec += k;
        sp->ck_pending += k;
        sp->ring_count = sp->ring_count > k ? sp->ring_count - k : 0;
        if (sp->tail_rec >= nrec) {
            sp->tail++;
            sp->tail_rec = 0;
        }
        if (sp->tail >= sp->head) {
            sp->ring_count = 0;
        }
    }
    sp->ck_max = sp->ck_pending > sp->ck_max ? sp->ck_pending : sp->ck_max;
    sp->peeked = false;
}
