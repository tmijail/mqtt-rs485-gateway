#include "gw_sfcard.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#if CONFIG_GW_SF_SPILL_SDMMC
#include "ff.h"   /* before diskio_sdmmc.h, which uses its types */
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "gw_bsp.h"
#include "sdmmc_cmd.h"
#endif

static const char *TAG = "sfcard";

/* QEMU stand-in: a "cluster" of 32 B makes one delete step (128 units) one 4 KiB LittleFS
 * block, so the E2E runs the segment limit and the stepwise delete on small files. */
#define LITTLEFS_UNIT 32u
/* The DMA bounce buffer for transfers from and to PSRAM: 8 sectors per command. */
#define DMA_BUF_BYTES 4096u

static char s_dir[16];

/* #122: open segment files. One segment is open in at most one slot (a second handle on the
 * same FAT file would not see the other's writes). A slot is chosen by its role when the
 * segment is not open yet. */
typedef struct {
    uint32_t seg;
    int fd;          /* -1: closed */
    uint32_t pos;    /* file position; UINT32_MAX = unknown */
} slot_t;
enum { SLOT_WRITE, SLOT_READ, SLOT_GC, SLOT_COUNT };
static slot_t s_slot[SLOT_COUNT] = {{0, -1, 0}, {0, -1, 0}, {0, -1, 0}};

static gw_sfcard_stats_t s_stats;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *const s_op_names[GW_SFCARD_OP_COUNT] = {
    "open", "append", "sync", "read", "mark", "step", "stat",
};

const char *gw_sfcard_op_name(gw_sfcard_op_t op)
{
    return op < GW_SFCARD_OP_COUNT ? s_op_names[op] : "?";
}

void gw_sfcard_get_stats(gw_sfcard_stats_t *out)
{
    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
}

static void timed(gw_sfcard_op_t op, int64_t t0, bool ok)
{
    int64_t d = esp_timer_get_time() - t0;
    uint32_t us = d > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)d;
    portENTER_CRITICAL(&s_stats_mux);
    gw_sfcard_time_t *t = &s_stats.op[op];
    t->count++;
    t->total_us += us;
    if (us > t->max_us) {
        t->max_us = us;
    }
    if (!ok) {
        s_stats.errors++;
    }
    portEXIT_CRITICAL(&s_stats_mux);
}

static void seg_path(char *out, size_t cap, uint32_t seg)
{
    snprintf(out, cap, "%s/%08lx.sf", s_dir, (unsigned long)seg);   /* an 8.3 name for FAT */
}

static slot_t *slot_find(uint32_t seg)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (s_slot[i].fd >= 0 && s_slot[i].seg == seg) {
            return &s_slot[i];
        }
    }
    return NULL;
}

static void slot_close(slot_t *s)
{
    if (s->fd >= 0) {
        int64_t t0 = esp_timer_get_time();
        bool ok = close(s->fd) == 0;
        timed(GW_SFCARD_OPEN, t0, ok);
        s->fd = -1;
    }
}

/* The open handle of segment @p seg, or a new one in the slot of @p role (the segment open
 * there before is closed). */
static slot_t *slot_get(uint32_t seg, int role, bool create)
{
    slot_t *s = slot_find(seg);
    if (s != NULL) {
        return s;
    }
    s = &s_slot[role];
    slot_close(s);
    char p[40];
    seg_path(p, sizeof(p), seg);
    int64_t t0 = esp_timer_get_time();
    int fd = open(p, O_RDWR | (create ? O_CREAT : 0), 0664);
    timed(GW_SFCARD_OPEN, t0, fd >= 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "%s: open failed (errno %d)", p, errno);
        return NULL;
    }
    s->seg = seg;
    s->fd = fd;
    s->pos = 0;
    return s;
}

static bool slot_seek(slot_t *s, uint32_t off)
{
    if (s->pos == off) {
        return true;
    }
    if (lseek(s->fd, (off_t)off, SEEK_SET) != (off_t)off) {
        s->pos = UINT32_MAX;
        return false;
    }
    s->pos = off;
    return true;
}

/* Write @p len bytes at @p off and sync them; op is the kind of the write (append or mark). */
static bool slot_write(slot_t *s, gw_sfcard_op_t op, uint32_t off, const void *data, size_t len)
{
    int64_t t0 = esp_timer_get_time();
    bool ok = slot_seek(s, off);
    const uint8_t *p = data;
    for (size_t left = len; ok && left > 0;) {
        ssize_t w = write(s->fd, p, left);
        ok = w > 0;
        if (ok) {
            p += w;
            left -= (size_t)w;
        }
    }
    timed(op, t0, ok);
    s->pos = ok ? off + (uint32_t)len : UINT32_MAX;
    if (ok) {
        t0 = esp_timer_get_time();
        ok = fsync(s->fd) == 0;
        timed(GW_SFCARD_SYNC, t0, ok);
    }
    if (!ok) {
        ESP_LOGE(TAG, "segment %08lx: write of %u bytes at %lu failed (errno %d)",
                 (unsigned long)s->seg, (unsigned)len, (unsigned long)off, errno);
        slot_close(s);
    }
    return ok;
}

static bool io_write(void *ctx, uint32_t seg, uint32_t offset, const void *data, size_t len)
{
    (void)ctx;
    slot_t *s = slot_get(seg, SLOT_WRITE, true);
    return s != NULL && slot_write(s, GW_SFCARD_APPEND, offset, data, len);
}

static bool io_patch(void *ctx, uint32_t seg, uint32_t offset, const void *data, size_t len)
{
    (void)ctx;
    slot_t *s = slot_get(seg, SLOT_GC, false);
    return s != NULL && slot_write(s, GW_SFCARD_MARK, offset, data, len);
}

static bool io_read(void *ctx, uint32_t seg, uint32_t offset, void *data, size_t len)
{
    (void)ctx;
    slot_t *s = slot_get(seg, SLOT_READ, false);
    if (s == NULL) {
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    bool ok = slot_seek(s, offset);
    uint8_t *p = data;
    for (size_t left = len; ok && left > 0;) {
        ssize_t r = read(s->fd, p, left);
        ok = r > 0;
        if (ok) {
            p += r;
            left -= (size_t)r;
        }
    }
    timed(GW_SFCARD_READ, t0, ok);
    s->pos = ok ? offset + (uint32_t)len : UINT32_MAX;
    if (!ok) {
        ESP_LOGE(TAG, "segment %08lx: read of %u bytes at %lu failed", (unsigned long)seg,
                 (unsigned)len, (unsigned long)offset);
    }
    return ok;
}

/* One delete step: the FAT frees the clusters after @p len (at most 128, gw_sfspill), and the
 * fsync writes the directory entry and the last FAT sector. */
static bool io_truncate(void *ctx, uint32_t seg, uint32_t len)
{
    (void)ctx;
    slot_t *s = slot_get(seg, SLOT_GC, false);
    if (s == NULL) {
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    bool ok = ftruncate(s->fd, (off_t)len) == 0 && fsync(s->fd) == 0;
    timed(GW_SFCARD_STEP, t0, ok);
    s->pos = UINT32_MAX;
    if (!ok) {
        ESP_LOGE(TAG, "segment %08lx: truncate to %lu failed (errno %d)", (unsigned long)seg,
                 (unsigned long)len, errno);
        slot_close(s);
    }
    return ok;
}

static bool io_remove(void *ctx, uint32_t seg)
{
    (void)ctx;
    slot_t *s = slot_find(seg);
    if (s != NULL) {
        slot_close(s);
    }
    char p[40];
    seg_path(p, sizeof(p), seg);
    int64_t t0 = esp_timer_get_time();
    bool ok = unlink(p) == 0 || errno == ENOENT;
    timed(GW_SFCARD_STEP, t0, ok);
    if (!ok) {
        ESP_LOGE(TAG, "%s: delete failed (errno %d)", p, errno);
    }
    return ok;
}

static int32_t io_size(void *ctx, uint32_t seg)
{
    (void)ctx;
    struct stat st;
    slot_t *s = slot_find(seg);
    if (s != NULL) {
        return fstat(s->fd, &st) == 0 ? (int32_t)st.st_size : -1;
    }
    char p[40];
    seg_path(p, sizeof(p), seg);
    int64_t t0 = esp_timer_get_time();
    int32_t size = stat(p, &st) == 0 ? (int32_t)st.st_size : -1;
    timed(GW_SFCARD_STAT, t0, true);   /* absent is an answer, not an error */
    return size;
}

static size_t io_list(void *ctx, uint32_t *ids, size_t max)
{
    (void)ctx;
    size_t n = 0;
    DIR *d = opendir(s_dir);
    for (struct dirent *e; d != NULL && n < max && (e = readdir(d)) != NULL;) {
        unsigned long id;
        char ext[4];
        if (sscanf(e->d_name, "%8lx.%3s", &id, ext) == 2 &&
            (strcmp(ext, "sf") == 0 || strcmp(ext, "SF") == 0)) {
            ids[n++] = (uint32_t)id;
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    return n;
}

bool gw_sfcard_open(gw_sfspill_io_t *io, char *where, size_t where_cap, char *why,
                    size_t why_cap)
{
    uint32_t unit = 0;
#if CONFIG_GW_SF_SPILL_SDMMC
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    /* #122: the ring is in PSRAM, which the SDMMC DMA cannot reach: without a buffer of its
     * own the driver allocates up to 8 KB of internal RAM for each transfer */
    void *dma = NULL;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        dma = heap_caps_malloc(DMA_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (dma != NULL) {
            host.dma_aligned_buffer = dma;
            host.unaligned_multi_block_rw_max_chunk_size = DMA_BUF_BYTES / 512u;
        }
    }
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = gw_bsp.sd.clk;
    slot.cmd = gw_bsp.sd.cmd;
    slot.d0 = gw_bsp.sd.d0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,   /* never erase a card that holds something else */
        .max_files = SLOT_COUNT,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card);
    if (err != ESP_OK) {
        heap_caps_free(dma);
        snprintf(why, why_cap, "microSD not mounted (%s): no card, or not FAT",
                 esp_err_to_name(err));
        return false;
    }
    /* the cluster size of the card (its format, not ours): the FatFs volume, without the
     * free-cluster count (f_getfree can read the whole FAT) */
    BYTE pdrv = ff_diskio_get_pdrv_card(card);
    const char drv[3] = {(char)('0' + pdrv), ':', 0};
    FF_DIR root;
    const char *type = "FAT";
    if (f_opendir(&root, drv) == FR_OK) {
        const FATFS *fs = root.obj.fs;
        unit = (uint32_t)fs->csize * fs->ssize;
        type = fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_FAT16 ? "FAT16" : "FAT12";
        if (fs->database % 8u != 0 || fs->csize % 8u != 0) {
            ESP_LOGW(TAG, "microSD: the clusters are not aligned to 4 KB (data area at sector "
                          "%lu, %u sectors per cluster): more flash writes per write",
                     (unsigned long)fs->database, (unsigned)fs->csize);
        }
        f_closedir(&root);
    }
    snprintf(s_dir, sizeof(s_dir), "/sdcard/sf");
    snprintf(where, where_cap, "microSD %s, %llu MB, %s, cluster %lu B", card->cid.name,
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024),
             type, (unsigned long)unit);
#elif CONFIG_GW_SF_SPILL_LITTLEFS
    /* QEMU build: the LittleFS partition (already mounted by gw_storage) stands in */
    unit = LITTLEFS_UNIT;
    snprintf(s_dir, sizeof(s_dir), "/cfg/sf");
    snprintf(where, where_cap, "LittleFS /cfg/sf (stand-in for the microSD card)");
#else
    (void)where;
    (void)where_cap;
    (void)unit;
    snprintf(why, why_cap, "no microSD support in this build (CONFIG_GW_SF_SPILL_NONE)");
    return false;
#endif
#if CONFIG_GW_SF_SPILL_SDMMC || CONFIG_GW_SF_SPILL_LITTLEFS
    if (mkdir(s_dir, 0775) != 0 && errno != EEXIST) {
        snprintf(why, why_cap, "%s: mkdir failed (errno %d)", s_dir, errno);
        return false;
    }
    s_stats.unit = unit;
    *io = (gw_sfspill_io_t){
        .ctx = NULL,
        .unit = unit,
        .write = io_write,
        .patch = io_patch,
        .read = io_read,
        .truncate = io_truncate,
        .remove = io_remove,
        .size = io_size,
        .list = io_list,
    };
    return true;
#endif
}
