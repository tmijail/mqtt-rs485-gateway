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
#include "freertos/task.h"
#include "gw_storage.h"
#include "sdkconfig.h"

#if CONFIG_GW_SF_SPILL_SDMMC
#include "ff.h"   /* before diskio_sdmmc.h, which uses its types */
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "gw_bsp.h"
#include "sdmmc_cmd.h"
#endif

static const char *TAG = "sfcard";

/* QEMU stand-in: a "cluster" of 32 B makes an extent (128 units) one 4 KiB LittleFS block, and
 * the delete step of a step 1 file 4 KiB, so the E2E runs many extents and the stepwise
 * delete on small files. */
#define LITTLEFS_UNIT 32u
/* The DMA bounce buffer for transfers from and to PSRAM: 8 sectors per command. */
#define DMA_BUF_BYTES 4096u
#define SECTOR 512u

static char s_dir[16];
#if CONFIG_GW_SF_SPILL_SDMMC
static void refresh_free(void);
#define REFRESH_FREE() refresh_free()
#else
#define REFRESH_FREE() ((void)0)
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
    "open", "block", "super", "read", "alloc", "step", "stat", "list",
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
    REFRESH_FREE();
    return 1;
}

static bool io_write(void *ctx, uint32_t f, uint32_t off, const void *data, size_t len)
{
    (void)ctx;
    gw_sfcard_op_t op = kind(f) == GW_SFSPILL_F_SUPER ? GW_SFCARD_SUPER : GW_SFCARD_BLOCK;
    slot_t *s = slot_get(f, SLOT_WRITE, false, NULL);
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
    REFRESH_FREE();
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
    REFRESH_FREE();
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


/* ---------- the card's state, its owner and the format (#139, #137) ---------- */

/* The format runs in a task of its own, created for the format and deleted after it (a card
 * is formatted once in its life: no RAM is kept for it in normal operation). Low priority, on
 * core 0, away from the bus task: it waits on the SDMMC transfers, so it uses little CPU. The
 * stack: the deepest path is f_mkfs -> disk_write -> sdmmc_write_sectors -> the host driver,
 * plus a log line; 5 KB leaves more than 2 KB free by the static estimate (docs/reference/architecture.md#tasks-queues-and-ownership). */
#define FORMAT_STACK_BYTES 5120
#define FORMAT_PRIORITY 1
#define RELEASE_WAIT_MS 10000u   /* the Sparkplug loop releases the card within one pass */
#define MIB (1024u * 1024u)

typedef enum { OWN_NONE, OWN_SPILL, OWN_FORMAT } owner_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   /* the fields below */
static owner_t s_owner;
static void (*s_release)(void);
static gw_cardfmt_t s_fmt;
static uint32_t s_fmt_stack_min;
static gw_card_info_t s_info = {
#if CONFIG_GW_SF_SPILL_SDMMC
    .state = GW_CARD_UNCHECKED,
    .detail = "the spill is off (store_forward.sd): the boot did not look for a card",
#elif CONFIG_GW_SF_SPILL_LITTLEFS
    .state = GW_CARD_UNSUPPORTED,
    .detail = "this build has no microSD card: LittleFS /cfg/sf stands in for it (QEMU)",
#else
    .state = GW_CARD_UNSUPPORTED,
    .detail = "no microSD support in this build (CONFIG_GW_SF_SPILL_NONE)",
#endif
    .free_mb = GW_CARD_UNKNOWN,
    .other = GW_CARD_UNKNOWN,
};

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void text_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void gw_sfcard_get_info(gw_card_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}

void gw_sfcard_get_format(gw_cardfmt_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_fmt;
    portEXIT_CRITICAL(&s_mux);
}

void gw_sfcard_format_stack(uint32_t *size, uint32_t *min_free)
{
    *size = FORMAT_STACK_BYTES;
    portENTER_CRITICAL(&s_mux);
    *min_free = s_fmt_stack_min;
    portEXIT_CRITICAL(&s_mux);
}

void gw_sfcard_unchecked(const char *why)
{
    portENTER_CRITICAL(&s_mux);
    if (s_info.state == GW_CARD_UNCHECKED) {
        text_copy(s_info.detail, sizeof(s_info.detail), why);
    }
    portEXIT_CRITICAL(&s_mux);
}

/* The spill gives the card up (I/O stopped, files closed). */
static void owner_drop(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_owner == OWN_SPILL) {
        s_owner = OWN_NONE;
        s_release = NULL;
    }
    portEXIT_CRITICAL(&s_mux);
}

void gw_sfcard_close(void)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        slot_close(&s_slot[i]);
    }
    owner_drop();
}

/* What a format made (done). */
typedef struct {
    char fat[6];
    uint32_t cluster;
    uint32_t size_mb;
} fmt_result_t;

#if CONFIG_GW_SF_SPILL_SDMMC
static sdmmc_card_t *s_card;   /* the boot mount: the card, its FAT volume, drive, DMA buffer */
static FATFS *s_fs;
static BYTE s_pdrv = FF_DRV_NOT_USED;
static void *s_dma;

static void info_put(const gw_card_info_t *c)
{
    portENTER_CRITICAL(&s_mux);
    s_info = *c;
    portEXIT_CRITICAL(&s_mux);
}

static void host_config(sdmmc_host_t *host, void *dma)
{
    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->slot = SDMMC_HOST_SLOT_1;
    if (dma != NULL) {
        host->dma_aligned_buffer = dma;
        host->unaligned_multi_block_rw_max_chunk_size = DMA_BUF_BYTES / SECTOR;
    }
}

/* Start the host and identify the card. On failure the host is stopped again. */
static esp_err_t card_probe(const sdmmc_host_t *host, sdmmc_card_t *card)
{
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = gw_bsp.sd.clk;
    slot.cmd = gw_bsp.sd.cmd;
    slot.d0 = gw_bsp.sd.d0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t err = host->init();
    if (err != ESP_OK) {
        return err;
    }
    err = sdmmc_host_init_slot(host->slot, &slot);
    if (err == ESP_OK) {
        err = sdmmc_card_init(host, card);
    }
    if (err != ESP_OK) {
        host->deinit_p(host->slot);
    }
    return err;
}

static uint32_t card_mb(const sdmmc_card_t *card)
{
    return (uint32_t)((uint64_t)card->csd.capacity * card->csd.sector_size / MIB);
}

static const char *fat_name(const FATFS *fs)
{
    return fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_FAT16 ? "FAT16" : "FAT12";
}

/* Why a card that answered does not mount. */
static const char *mount_reason(FRESULT fr)
{
    switch (fr) {
    case FR_NO_FILESYSTEM:
        return "no FAT file system (exFAT, NTFS, ext4, GPT, or no format)";
    case FR_DISK_ERR:
    case FR_NOT_READY:
        return "the card does not read";
    case FR_NOT_ENOUGH_CORE:
        return "no memory for the file system";
    default:
        return "FatFs refused it";
    }
}

/* The free space that FatFs knows without a scan of the FAT (FSInfo on FAT32, kept up to date
 * as clusters are allocated), or GW_CARD_UNKNOWN. */
static uint32_t free_mb(const FATFS *fs)
{
    if (fs == NULL || fs->n_fatent < 2 || fs->free_clst > fs->n_fatent - 2) {
        return GW_CARD_UNKNOWN;
    }
    return (uint32_t)((uint64_t)fs->free_clst * fs->csize * fs->ssize / MIB);
}

/* The owner (Sparkplug task) after an allocation or a delete: the free space for the status. */
static void refresh_free(void)
{
    uint32_t mb = free_mb(s_fs);
    portENTER_CRITICAL(&s_mux);
    if (s_info.state == GW_CARD_MOUNTED) {
        s_info.free_mb = mb;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* Entries of the root other than our folder "sf" (bounded): the format would delete them. */
static uint32_t count_other(const char *drv)
{
    FF_DIR d;
    FILINFO fi;
    uint32_t n = 0;
    if (f_opendir(&d, drv) != FR_OK) {
        return GW_CARD_UNKNOWN;
    }
    for (int i = 0; i < 1000 && f_readdir(&d, &fi) == FR_OK && fi.fname[0] != '\0'; i++) {
        if (strcasecmp(fi.fname, "sf") != 0) {
            n++;
        }
    }
    f_closedir(&d);
    return n;
}

/* The FAT type and the cluster size of the mounted card, with no free-cluster count
 * (f_getfree can read the whole FAT). */
static uint32_t card_geometry(const FATFS *fs, const char **type)
{
    *type = fat_name(fs);
    if (fs->database % 8u != 0 || fs->csize % 8u != 0) {
        ESP_LOGW(TAG, "microSD: the clusters are not aligned to 4 KB (data area at sector %lu, %u "
                      "sectors per cluster): more flash writes per block; format the card with "
                      "the gateway",
                 (unsigned long)fs->database, (unsigned)fs->csize);
    }
    return (uint32_t)fs->csize * fs->ssize;
}

/* The boot mount, released before a format (the spill gave the card up). */
static void unmount_boot(void)
{
    if (s_card == NULL) {
        return;
    }
    const char drv[3] = {(char)('0' + s_pdrv), ':', 0};
    f_mount(NULL, drv, 0);
    esp_vfs_fat_unregister_path("/sdcard");   /* frees s_fs */
    ff_diskio_unregister(s_pdrv);
    s_card->host.deinit_p(s_card->host.slot);
    heap_caps_free(s_card);
    heap_caps_free(s_dma);
    s_card = NULL;
    s_fs = NULL;
    s_dma = NULL;
    s_pdrv = FF_DRV_NOT_USED;
}

/*
 * The format itself, in the format task, with the card free (no owner). It does what an
 * explicit, confirmed request asked for, also on a card that does not mount: it starts the
 * host and the card itself (the boot never formats: no format_if_mount_failed anywhere).
 *   - The primary and the backup GPT headers (sector 1 and the last sector) are zeroed, so
 *     that no tool takes the card for a GPT disk afterwards (FatFs is built without GPT).
 *   - f_mkfs writes a new MBR with one partition over the whole card (FatFs puts it at sector
 *     63) and FAT32 with 32 KiB clusters; below 2 GB, where FAT32 cannot have that many
 *     clusters, FAT16. Its data area starts at a multiple of 4 MiB (MKFS_PARM.align), the
 *     allocation unit of SDHC/SDXC cards: every cluster, and every extent of the ring
 *     (128 clusters), is aligned to the card's erase blocks.
 *   - The new volume is mounted once to check it and read its geometry.
 * The buffers come from internal DMA-capable RAM (no bounce buffer: the FAT is cleared in
 * writes of 16 KiB), for the format only.
 */
static bool format_card(fmt_result_t *r, char *why, size_t cap)
{
    gw_card_info_t c = {.state = GW_CARD_FORMATTING, .free_mb = GW_CARD_UNKNOWN,
                        .other = GW_CARD_UNKNOWN};
    unmount_boot();
    info_put(&c);
    size_t wsz = 16384;
    uint8_t *work = heap_caps_malloc(wsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (work == NULL) {
        wsz = 4096;
        work = heap_caps_malloc(wsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    sdmmc_card_t *card = heap_caps_calloc(1, sizeof(*card), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    FATFS *fs = heap_caps_calloc(1, sizeof(*fs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    sdmmc_host_t host;
    host_config(&host, NULL);
    BYTE pdrv = FF_DRV_NOT_USED;
    bool probed = false, ok = false;
    esp_err_t err;
    FRESULT fr;
    if (work == NULL || card == NULL || fs == NULL) {
        snprintf(why, cap, "no memory for the format buffers: nothing formatted");
        goto out;
    }
    err = card_probe(&host, card);
    if (err != ESP_OK) {
        snprintf(why, cap, "no card answers (%s): nothing formatted", esp_err_to_name(err));
        goto out;
    }
    probed = true;
    text_copy(c.name, sizeof(c.name), card->cid.name);
    c.size_mb = card_mb(card);
    info_put(&c);
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        pdrv = FF_DRV_NOT_USED;
        snprintf(why, cap, "no free FatFs drive: nothing formatted");
        goto out;
    }
    ff_diskio_register_sdmmc(pdrv, card);
    const char drv[3] = {(char)('0' + pdrv), ':', 0};
    ESP_LOGW(TAG, "card format: microSD %s, %lu MB: one MBR partition, FAT with 32 KB clusters",
             card->cid.name, (unsigned long)c.size_mb);
    if (card->csd.sector_size == SECTOR && card->csd.capacity > 2) {
        memset(work, 0, SECTOR);
        err = sdmmc_write_sectors(card, work, 1, 1);
        if (err == ESP_OK) {
            err = sdmmc_write_sectors(card, work, (size_t)card->csd.capacity - 1, 1);
        }
        if (err != ESP_OK) {
            snprintf(why, cap, "write error (%s): the card may be write-protected or worn out",
                     esp_err_to_name(err));
            goto out;
        }
    }
    const MKFS_PARM opt = {
        .fmt = FM_FAT | FM_FAT32,
        .n_fat = 2,
        .align = 4u * MIB / SECTOR,
        .n_root = 0,
        .au_size = 32 * 1024,   /* #124: 32 KiB clusters */
    };
    fr = f_mkfs(drv, &opt, work, (UINT)wsz);
    if (fr != FR_OK) {
        snprintf(why, cap, "f_mkfs failed (FatFs error %d)%s", (int)fr,
                 fr == FR_MKFS_ABORTED ? ": the card is too small for FAT with 32 KB clusters"
                 : fr == FR_DISK_ERR   ? ": write error"
                                       : "");
        goto out;
    }
    fr = f_mount(fs, drv, 1);
    if (fr != FR_OK) {
        snprintf(why, cap, "the new file system does not mount (FatFs error %d)", (int)fr);
        goto out;
    }
    text_copy(r->fat, sizeof(r->fat), fat_name(fs));
    r->cluster = (uint32_t)fs->csize * fs->ssize;
    r->size_mb = (uint32_t)((uint64_t)(fs->n_fatent - 2) * r->cluster / MIB);
    f_mount(NULL, drv, 0);
    ok = true;
out:
    if (pdrv != FF_DRV_NOT_USED) {
        ff_diskio_unregister(pdrv);
    }
    if (probed) {
        host.deinit_p(host.slot);
    }
    heap_caps_free(fs);
    heap_caps_free(card);
    heap_caps_free(work);
    if (ok) {
        c.state = GW_CARD_FORMATTED;
        text_copy(c.fat, sizeof(c.fat), r->fat);
        c.cluster = r->cluster;
        c.free_mb = r->size_mb;
        c.other = 0;
        text_copy(c.detail, sizeof(c.detail), "restart the gateway to use it");
    } else {
        c.state = probed ? GW_CARD_UNMOUNTED : GW_CARD_ABSENT;
        text_copy(c.detail, sizeof(c.detail), probed ? "the format failed" : why);
    }
    info_put(&c);
    return ok;
}
#endif

/* Take the card for the format: a spill that uses it is asked to stop (its files are closed
 * by the Sparkplug task, at a point between two card operations), and the format waits for
 * that. Before the boot mounts the card, the format takes it at once, and the mount then
 * refuses (the spill stays off until the restart). */
static bool claim_card(char *why, size_t cap)
{
    for (uint32_t waited = 0;; waited += 50) {
        portENTER_CRITICAL(&s_mux);
        owner_t o = s_owner;
        void (*release)(void) = s_release;
        if (o != OWN_SPILL) {
            s_owner = OWN_FORMAT;
        }
        portEXIT_CRITICAL(&s_mux);
        if (o != OWN_SPILL) {
            return true;
        }
        if (waited == 0 && release != NULL) {
            ESP_LOGW(TAG, "card format: the spill uses the card: the Sparkplug task stops it first");
            release();
        }
        if (waited >= RELEASE_WAIT_MS) {
            snprintf(why, cap, "the Sparkplug task did not release the card in %u s: nothing "
                               "formatted",
                     (unsigned)(RELEASE_WAIT_MS / 1000));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void format_task(void *arg)
{
    uint32_t sim_ms = (uint32_t)(uintptr_t)arg;
    char why[96] = "";
    fmt_result_t r = {.fat = "FAT32", .cluster = 32 * 1024};
    bool ok = claim_card(why, sizeof(why));
    if (ok && sim_ms > 0) {
        /* CONFIG_GW_IO_SIMULATED: the card is released, nothing is written */
        for (uint32_t t = 0; t < sim_ms; t += 100) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } else if (ok) {
#if CONFIG_GW_SF_SPILL_SDMMC
        ok = format_card(&r, why, sizeof(why));
#else
        ok = false;   /* refused before the task in such a build */
        snprintf(why, sizeof(why), "%s", gw_cardfmt_answer_text(GW_CARDFMT_NO_SUPPORT));
#endif
    }
    uint32_t now = now_ms();
    uint32_t stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    gw_cardfmt_t st;
    portENTER_CRITICAL(&s_mux);
    if (ok) {
        gw_cardfmt_done(&s_fmt, now, r.fat, r.cluster, r.size_mb);
    } else {
        gw_cardfmt_failed(&s_fmt, now, why);
    }
    if (s_owner == OWN_FORMAT) {
        s_owner = OWN_NONE;
    }
    s_fmt_stack_min = stack_free;
    st = s_fmt;
    portEXIT_CRITICAL(&s_mux);
    char line[200];
    gw_cardfmt_text(&st, now, line, sizeof(line));
    if (ok) {
        ESP_LOGW(TAG, "card format: %s (format task: %lu of %u B of stack never used)", line,
                 (unsigned long)stack_free, FORMAT_STACK_BYTES);
    } else {
        ESP_LOGE(TAG, "card format: %s (format task: %lu of %u B of stack never used)", line,
                 (unsigned long)stack_free, FORMAT_STACK_BYTES);
    }
    vTaskDelete(NULL);
}

static gw_cardfmt_answer_t format_start(uint32_t sim_ms)
{
#if CONFIG_GW_SF_SPILL_SDMMC
    const bool support = true;
#else
    const bool support = false;
#endif
    bool psram = gw_storage_psram();
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_mux);
    gw_cardfmt_t before = s_fmt;
    gw_cardfmt_answer_t a = gw_cardfmt_request(&s_fmt, support, psram, sim_ms > 0, now);
    portEXIT_CRITICAL(&s_mux);
    if (a != GW_CARDFMT_STARTED) {
        return a;
    }
    if (xTaskCreatePinnedToCore(format_task, "card_format", FORMAT_STACK_BYTES,
                                (void *)(uintptr_t)sim_ms, FORMAT_PRIORITY, NULL, 0) != pdPASS) {
        portENTER_CRITICAL(&s_mux);
        s_fmt = before;
        portEXIT_CRITICAL(&s_mux);
        return GW_CARDFMT_NO_MEMORY;
    }
    ESP_LOGW(TAG, "card format requested%s: it runs in the background",
             sim_ms > 0 ? " (simulated: nothing is written)" : "");
    return a;
}

gw_cardfmt_answer_t gw_sfcard_format_start(void)
{
    return format_start(0);
}

#if CONFIG_GW_IO_SIMULATED
gw_cardfmt_answer_t gw_sfcard_format_simulate(uint32_t ms)
{
    return format_start(ms > 0 ? ms : 1);
}
#endif

/* The spill takes the card at boot, unless a format runs. */
static bool owner_take(void (*release)(void))
{
    portENTER_CRITICAL(&s_mux);
    bool taken = s_owner != OWN_FORMAT;
    if (taken) {
        s_owner = OWN_SPILL;
        s_release = release;
    }
    portEXIT_CRITICAL(&s_mux);
    return taken;
}

bool gw_sfcard_open(gw_sfspill_io_t *io, char *where, size_t where_cap, char *why,
                    size_t why_cap, void (*release)(void))
{
    uint32_t unit = 0;
    if (!owner_take(release)) {
        snprintf(why, why_cap, "a format of the card runs");
        return false;
    }
#if CONFIG_GW_SF_SPILL_SDMMC
    /* #122: the ring is in PSRAM, which the SDMMC DMA cannot reach: without a buffer of its
     * own the driver allocates up to 8 KB of internal RAM for each transfer. (The spill starts
     * only with the PSRAM: gw_edge's setup_spill.) */
    void *dma = heap_caps_malloc(DMA_BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    sdmmc_host_t host;
    host_config(&host, dma);
    sdmmc_card_t *card = heap_caps_calloc(1, sizeof(*card), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = card != NULL ? card_probe(&host, card) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        heap_caps_free(card);
        heap_caps_free(dma);
        owner_drop();
        snprintf(why, why_cap, "no microSD card answers (%s)", esp_err_to_name(err));
        gw_card_info_t c = {.state = GW_CARD_ABSENT, .free_mb = GW_CARD_UNKNOWN,
                            .other = GW_CARD_UNKNOWN};
        snprintf(c.detail, sizeof(c.detail), "no card, or it does not answer (%s)",
                 esp_err_to_name(err));
        info_put(&c);
        return false;
    }
    /* The VFS and FatFs, as esp_vfs_fat_sdmmc_mount() does, but never with a format: a card
     * that does not mount stays as it is, and the reason goes to the card's state */
    BYTE pdrv = FF_DRV_NOT_USED;
    FATFS *fs = NULL;
    FRESULT fr = FR_NOT_ENOUGH_CORE;
    if (ff_diskio_get_drive(&pdrv) == ESP_OK && pdrv != FF_DRV_NOT_USED) {
        ff_diskio_register_sdmmc(pdrv, card);
        const char drv[3] = {(char)('0' + pdrv), ':', 0};
        const esp_vfs_fat_conf_t conf = {.base_path = "/sdcard", .fat_drive = drv,
                                         .max_files = SLOT_COUNT};
        if (esp_vfs_fat_register(&conf, &fs) == ESP_OK) {
            fr = f_mount(fs, drv, 1);
        }
        if (fr != FR_OK) {
            if (fs != NULL) {
                f_mount(NULL, drv, 0);
                esp_vfs_fat_unregister_path("/sdcard");
            }
            ff_diskio_unregister(pdrv);
        }
    }
    gw_card_info_t c = {.state = GW_CARD_MOUNTED, .free_mb = GW_CARD_UNKNOWN,
                        .other = GW_CARD_UNKNOWN};
    text_copy(c.name, sizeof(c.name), card->cid.name);
    c.size_mb = card_mb(card);
    if (fr != FR_OK) {
        host.deinit_p(host.slot);
        heap_caps_free(card);
        heap_caps_free(dma);
        owner_drop();
        c.state = GW_CARD_UNMOUNTED;
        snprintf(c.detail, sizeof(c.detail), "%s (FatFs error %d)", mount_reason(fr), (int)fr);
        info_put(&c);
        snprintf(why, why_cap, "microSD not mounted: %s", mount_reason(fr));
        ESP_LOGE(TAG, "microSD %s, %lu MB: not mounted: %s (FatFs error %d). Format it with the "
                      "gateway: web UI, or shell 'card format confirm' (it deletes everything "
                      "on the card)",
                 c.name, (unsigned long)c.size_mb, mount_reason(fr), (int)fr);
        return false;
    }
    s_card = card;
    s_fs = fs;
    s_pdrv = pdrv;
    s_dma = dma;
    const char drv[3] = {(char)('0' + pdrv), ':', 0};
    const char *type;
    unit = card_geometry(fs, &type);
    text_copy(c.fat, sizeof(c.fat), type);
    c.cluster = unit;
    c.free_mb = free_mb(fs);
    c.other = count_other(drv);
    info_put(&c);
    snprintf(s_dir, sizeof(s_dir), "/sdcard/sf");
    snprintf(where, where_cap, "microSD %s, %lu MB, %s, cluster %lu B", c.name,
             (unsigned long)c.size_mb, type, (unsigned long)unit);
#elif CONFIG_GW_SF_SPILL_LITTLEFS
    /* QEMU build: the LittleFS partition (already mounted by gw_storage) stands in */
    unit = LITTLEFS_UNIT;
    snprintf(s_dir, sizeof(s_dir), "/cfg/sf");
    snprintf(where, where_cap, "LittleFS /cfg/sf (stand-in for the microSD card)");
#else
    (void)where;
    (void)where_cap;
    (void)unit;
    owner_drop();
    snprintf(why, why_cap, "no microSD support in this build (CONFIG_GW_SF_SPILL_NONE)");
    return false;
#endif
#if CONFIG_GW_SF_SPILL_SDMMC || CONFIG_GW_SF_SPILL_LITTLEFS
    if (mkdir(s_dir, 0775) != 0 && errno != EEXIST) {
        snprintf(why, why_cap, "%s: mkdir failed (errno %d)", s_dir, errno);
        owner_drop();
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
