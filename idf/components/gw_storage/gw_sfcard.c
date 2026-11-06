#include "gw_sfcard.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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

/* QEMU stand-in: a "cluster" of 32 B makes an extent (128 units) one 4 KiB LittleFS block, and
 * the delete step of an old segment 4 KiB, so the E2E runs many extents and the stepwise
 * delete on small files. */
#define LITTLEFS_UNIT 32u
/* The DMA bounce buffer for transfers from and to PSRAM: 8 sectors per command. */
#define DMA_BUF_BYTES 4096u
#define SECTOR 512u

static char s_dir[16];
static bool s_mounted;
#if CONFIG_GW_SF_SPILL_SDMMC
static sdmmc_card_t *s_card;
#endif

/* Open files. One file is open in at most one slot (a second handle on the same FAT file would
 * not see the other's writes). A slot is chosen by its role when the file is not open yet. */
typedef struct {
    uint32_t f;
    int fd;          /* -1: closed */
    uint32_t pos;    /* file position; UINT32_MAX = unknown */
} slot_t;
enum { SLOT_WRITE, SLOT_READ, SLOT_GC, SLOT_SB_A, SLOT_SB_B, SLOT_COUNT };
static slot_t s_slot[SLOT_COUNT] = {
    {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0}, {0, -1, 0},
};

static gw_sfcard_stats_t s_stats;
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *const s_op_names[GW_SFCARD_OP_COUNT] = {
    "open", "block", "super", "read", "alloc", "mark", "step", "stat", "list",
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

static uint32_t kind(uint32_t f)
{
    return f & GW_SFSPILL_F_KIND;
}

/* The file's 8.3 name (#122: no long-file-name buffer on FAT). */
static void file_path(char *out, size_t cap, uint32_t f)
{
    if (kind(f) == GW_SFSPILL_F_SUPER) {
        snprintf(out, cap, "%s/ring%c.sf", s_dir, (f & 1u) ? 'b' : 'a');
    } else if (kind(f) == GW_SFSPILL_F_EXTENT) {
        snprintf(out, cap, "%s/r%04lx.sf", s_dir, (unsigned long)(f & 0xFFFFu));
    } else {
        snprintf(out, cap, "%s/%08lx.sf", s_dir, (unsigned long)f);
    }
}

static slot_t *slot_find(uint32_t f)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (s_slot[i].fd >= 0 && s_slot[i].f == f) {
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

void gw_sfcard_close(void)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        slot_close(&s_slot[i]);
    }
}

static int role_of(uint32_t f, int other)
{
    if (kind(f) == GW_SFSPILL_F_SUPER) {
        return (f & 1u) ? SLOT_SB_B : SLOT_SB_A;
    }
    return other;
}

/* The open handle of file @p f, or a new one in the slot of @p role (the file open there before
 * is closed). NULL with *absent set when the file does not exist (and @p create is false). */
static slot_t *slot_get(uint32_t f, int role, bool create, bool *absent)
{
    if (absent != NULL) {
        *absent = false;
    }
    slot_t *s = slot_find(f);
    if (s != NULL) {
        return s;
    }
    s = &s_slot[role_of(f, role)];
    slot_close(s);
    char p[40];
    file_path(p, sizeof(p), f);
    int64_t t0 = esp_timer_get_time();
    int fd = open(p, O_RDWR | (create ? O_CREAT : 0), 0664);
    bool gone = fd < 0 && errno == ENOENT;
    timed(GW_SFCARD_OPEN, t0, fd >= 0 || gone);
    if (fd < 0) {
        if (gone && absent != NULL) {
            *absent = true;
        } else {
            ESP_LOGE(TAG, "%s: open failed (errno %d)", p, errno);
        }
        return NULL;
    }
    s->f = f;
    s->fd = fd;
    s->pos = 0;
    return s;
}

static int32_t slot_size(const slot_t *s)
{
    struct stat st;
    return fstat(s->fd, &st) == 0 ? (int32_t)st.st_size : -1;
}

/* Never past the end: on a file open for writing, FatFs would extend it. */
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

static int io_alloc(void *ctx, uint32_t f, uint32_t size)
{
    (void)ctx;
    slot_t *s = slot_get(f, SLOT_WRITE, true, NULL);
    if (s == NULL) {
        return -1;
    }
    int32_t have = slot_size(s);
    if (have >= 0 && (uint32_t)have >= size) {
        return 1;   /* whole already (a later boot, or a ring reset): nothing to change */
    }
    int64_t t0 = esp_timer_get_time();
#if CONFIG_GW_SF_SPILL_LITTLEFS
    /* LittleFS extends with zeros (and copies on write): fine for the stand-in */
    bool ok = ftruncate(s->fd, (off_t)size) == 0;
    bool full = !ok && errno == ENOSPC;
    ok = ok && fsync(s->fd) == 0;
#else
    /* FAT: a seek past the end of a file open for writing allocates the clusters (it does not
     * write them), the fsync writes the FAT and the directory entry; at most 128 clusters */
    bool ok = lseek(s->fd, (off_t)size, SEEK_SET) >= 0 && fsync(s->fd) == 0;
    bool full = false;
#endif
    int32_t now = ok ? slot_size(s) : -1;
    s->pos = UINT32_MAX;
    if (ok && now >= 0 && (uint32_t)now < size) {
        full = true;   /* FatFs stops at the last free cluster */
    }
    timed(GW_SFCARD_ALLOC, t0, ok || full);
    if (full) {
        ESP_LOGW(TAG, "file %08lx: card full (%ld of %lu bytes allocated)", (unsigned long)f,
                 (long)now, (unsigned long)size);
        return 0;
    }
    if (!ok) {
        ESP_LOGE(TAG, "file %08lx: allocation of %lu bytes failed (errno %d)", (unsigned long)f,
                 (unsigned long)size, errno);
        slot_close(s);
        return -1;
    }
    return 1;
}

static bool io_write(void *ctx, uint32_t f, uint32_t off, const void *data, size_t len)
{
    (void)ctx;
    gw_sfcard_op_t op = kind(f) == GW_SFSPILL_F_SUPER    ? GW_SFCARD_SUPER
                        : kind(f) == GW_SFSPILL_F_EXTENT ? GW_SFCARD_BLOCK
                                                         : GW_SFCARD_MARK;
    slot_t *s = slot_get(f, op == GW_SFCARD_MARK ? SLOT_GC : SLOT_WRITE, false, NULL);
    if (s == NULL) {
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    int32_t size = slot_size(s);
    bool ok = size >= 0 && (uint64_t)off + len <= (uint64_t)size && slot_seek(s, off);
    const uint8_t *p = data;
    for (size_t left = len; ok && left > 0;) {
        ssize_t w = write(s->fd, p, left);
        ok = w > 0;
        if (ok) {
            p += w;
            left -= (size_t)w;
        }
    }
    s->pos = ok ? off + (uint32_t)len : UINT32_MAX;
#if CONFIG_GW_SF_SPILL_LITTLEFS
    bool sync = true;   /* LittleFS keeps a write in its cache until the sync */
#else
    /* whole aligned sectors went straight to the card; a part of one is in the FIL buffer */
    bool sync = off % SECTOR != 0 || len % SECTOR != 0;
#endif
    if (ok && sync) {
        ok = fsync(s->fd) == 0;
    }
    timed(op, t0, ok);
    if (!ok) {
        ESP_LOGE(TAG, "file %08lx: write of %u bytes at %lu failed (errno %d)", (unsigned long)f,
                 (unsigned)len, (unsigned long)off, errno);
        slot_close(s);
    }
    return ok;
}

static int io_read(void *ctx, uint32_t f, uint32_t off, void *data, size_t len)
{
    (void)ctx;
    bool absent;
    slot_t *s = slot_get(f, SLOT_READ, false, &absent);
    if (s == NULL) {
        return absent ? 0 : -1;
    }
    int64_t t0 = esp_timer_get_time();
    int32_t size = slot_size(s);
    if (size < 0) {
        timed(GW_SFCARD_READ, t0, false);
        return -1;
    }
    if ((uint64_t)off + len > (uint64_t)size) {
        timed(GW_SFCARD_READ, t0, true);
        return 0;   /* shorter than that (an extent a power cut left short) */
    }
    bool ok = slot_seek(s, off);
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
    s->pos = ok ? off + (uint32_t)len : UINT32_MAX;
    if (!ok) {
        ESP_LOGE(TAG, "file %08lx: read of %u bytes at %lu failed", (unsigned long)f,
                 (unsigned)len, (unsigned long)off);
        slot_close(s);
        return -1;
    }
    return 1;
}

/* One delete step: the FAT frees the clusters after @p len (at most 128), and the fsync writes
 * the directory entry and the last FAT sector. */
static bool io_truncate(void *ctx, uint32_t f, uint32_t len)
{
    (void)ctx;
    slot_t *s = slot_get(f, SLOT_GC, false, NULL);
    if (s == NULL) {
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    bool ok = ftruncate(s->fd, (off_t)len) == 0 && fsync(s->fd) == 0;
    timed(GW_SFCARD_STEP, t0, ok);
    s->pos = UINT32_MAX;
    if (!ok) {
        ESP_LOGE(TAG, "file %08lx: truncate to %lu failed (errno %d)", (unsigned long)f,
                 (unsigned long)len, errno);
        slot_close(s);
    }
    return ok;
}

static bool io_remove(void *ctx, uint32_t f)
{
    (void)ctx;
    slot_t *s = slot_find(f);
    if (s != NULL) {
        slot_close(s);
    }
    char p[40];
    file_path(p, sizeof(p), f);
    int64_t t0 = esp_timer_get_time();
    bool ok = unlink(p) == 0 || errno == ENOENT;
    timed(GW_SFCARD_STEP, t0, ok);
    if (!ok) {
        ESP_LOGE(TAG, "%s: delete failed (errno %d)", p, errno);
    }
    return ok;
}

static int32_t io_size(void *ctx, uint32_t f)
{
    (void)ctx;
    slot_t *s = slot_find(f);
    if (s != NULL) {
        return slot_size(s);
    }
    char p[40];
    file_path(p, sizeof(p), f);
    struct stat st;
    int64_t t0 = esp_timer_get_time();
    int32_t size = stat(p, &st) == 0 ? (int32_t)st.st_size : -1;
    timed(GW_SFCARD_STAT, t0, true);   /* absent is an answer, not an error */
    return size;
}

static bool hex_name(const char *s, size_t n, unsigned long *v)
{
    *v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        if (d < 0) {
            return false;
        }
        *v = *v * 16u + (unsigned long)d;
    }
    return true;
}

/* Our files: ringa.sf / ringb.sf, rXXXX.sf, NNNNNNNN.sf (below 0x80000000: the others are not
 * ours and are never touched, #134). */
static bool io_list(void *ctx, void (*each)(void *arg, uint32_t f), void *arg)
{
    (void)ctx;
    int64_t t0 = esp_timer_get_time();
    DIR *d = opendir(s_dir);
    for (struct dirent *e; d != NULL && (e = readdir(d)) != NULL;) {
        const char *n = e->d_name;
        size_t len = strlen(n);
        unsigned long v;
        if (len < 4 || strcasecmp(n + len - 3, ".sf") != 0) {
            continue;
        }
        if (len == 8 && strncasecmp(n, "ring", 4) == 0 && (n[4] == 'a' || n[4] == 'A' ||
                                                           n[4] == 'b' || n[4] == 'B')) {
            each(arg, GW_SFSPILL_F_SUPER | ((n[4] == 'b' || n[4] == 'B') ? 1u : 0u));
        } else if (len == 8 && (n[0] == 'r' || n[0] == 'R') && hex_name(n + 1, 4, &v)) {
            each(arg, GW_SFSPILL_F_EXTENT | (uint32_t)v);
        } else if (len == 11 && hex_name(n, 8, &v) && v < GW_SFSPILL_ID_LIMIT) {
            each(arg, (uint32_t)v);
        }
    }
    bool ok = d != NULL;
    if (d != NULL) {
        closedir(d);
    }
    timed(GW_SFCARD_LIST, t0, ok);
    return ok;
}

#if CONFIG_GW_SF_SPILL_SDMMC
/* The FAT type and the cluster size of the mounted card, without the free-cluster count
 * (f_getfree can read the whole FAT). */
static uint32_t card_geometry(const char **type)
{
    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    const char drv[3] = {(char)('0' + pdrv), ':', 0};
    FF_DIR root;
    uint32_t unit = 0;
    *type = "FAT";
    if (f_opendir(&root, drv) == FR_OK) {
        const FATFS *fs = root.obj.fs;
        unit = (uint32_t)fs->csize * fs->ssize;
        *type = fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_FAT16 ? "FAT16" : "FAT12";
        if (fs->database % 8u != 0 || fs->csize % 8u != 0) {
            ESP_LOGW(TAG, "microSD: the clusters are not aligned to 4 KB (data area at sector "
                          "%lu, %u sectors per cluster): more flash writes per block",
                     (unsigned long)fs->database, (unsigned)fs->csize);
        }
        f_closedir(&root);
    }
    return unit;
}
#endif

bool gw_sfcard_format(char *msg, size_t cap)
{
#if CONFIG_GW_SF_SPILL_SDMMC
    if (!s_mounted) {
        snprintf(msg, cap, "no microSD card is mounted (store_forward.sd is off, there is no "
                           "PSRAM, or the mount failed): nothing formatted");
        return false;
    }
    gw_sfcard_close();
    esp_vfs_fat_mount_config_t mc = {
        .format_if_mount_failed = false,
        .max_files = SLOT_COUNT,
        .allocation_unit_size = 32 * 1024,   /* #124: 32 KiB clusters */
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_vfs_fat_sdcard_format_cfg("/sdcard", s_card, &mc);
    if (err != ESP_OK) {
        s_mounted = false;
        snprintf(msg, cap, "format failed (%s): remove the card or restart", esp_err_to_name(err));
        return false;
    }
    const char *type;
    uint32_t unit = card_geometry(&type);
    mkdir(s_dir, 0775);
    snprintf(msg, cap, "formatted in %lu s: %s, %lu B clusters, %llu MB; restart the gateway to "
                       "use the card",
             (unsigned long)((esp_timer_get_time() - t0) / 1000000), type, (unsigned long)unit,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    return true;
#elif CONFIG_GW_SF_SPILL_LITTLEFS
    snprintf(msg, cap, "this build has no microSD card: a LittleFS directory stands in for it "
                       "(QEMU); nothing formatted");
    return false;
#else
    snprintf(msg, cap, "no microSD support in this build (CONFIG_GW_SF_SPILL_NONE): nothing "
                       "formatted");
    return false;
#endif
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
            host.unaligned_multi_block_rw_max_chunk_size = DMA_BUF_BYTES / SECTOR;
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
        .allocation_unit_size = 32 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &s_card);
    if (err != ESP_OK) {
        heap_caps_free(dma);
        snprintf(why, why_cap, "microSD not mounted (%s): no card, or not FAT (exFAT is not "
                               "supported)",
                 esp_err_to_name(err));
        return false;
    }
    s_mounted = true;
    const char *type;
    unit = card_geometry(&type);
    snprintf(s_dir, sizeof(s_dir), "/sdcard/sf");
    snprintf(where, where_cap, "microSD %s, %llu MB, %s, cluster %lu B", s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024),
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
        .alloc = io_alloc,
        .write = io_write,
        .read = io_read,
        .truncate = io_truncate,
        .remove = io_remove,
        .size = io_size,
        .list = io_list,
    };
    return true;
#endif
}
