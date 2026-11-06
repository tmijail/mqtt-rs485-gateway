#include "gw_sfcard.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_GW_SF_SPILL_SDMMC
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "gw_bsp.h"
#include "sdmmc_cmd.h"
#endif

static const char *TAG = "sfcard";

static char s_dir[16];

static void seg_path(char *out, size_t cap, uint32_t seg)
{
    snprintf(out, cap, "%s/%08lx.sf", s_dir, (unsigned long)seg);   /* an 8.3 name for FAT */
}

/* One open/write/sync/close per chunk: the spill writes a few KB at a time, and only while the
 * RAM ring is full, so there is no file handle to lose on a power cut. */
static bool io_append(void *ctx, uint32_t seg, const void *data, size_t len)
{
    (void)ctx;
    char p[40];
    seg_path(p, sizeof(p), seg);
    FILE *f = fopen(p, "ab");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s: open for append failed (errno %d)", p, errno);
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        ESP_LOGE(TAG, "%s: write failed (errno %d)", p, errno);
    }
    return ok;
}

static bool io_read(void *ctx, uint32_t seg, uint32_t offset, void *data, size_t len)
{
    (void)ctx;
    char p[40];
    seg_path(p, sizeof(p), seg);
    FILE *f = fopen(p, "rb");
    bool ok = f != NULL && fseek(f, (long)offset, SEEK_SET) == 0 && fread(data, 1, len, f) == len;
    if (f != NULL) {
        fclose(f);
    }
    if (!ok) {
        ESP_LOGE(TAG, "%s: read of %u bytes at %lu failed", p, (unsigned)len,
                 (unsigned long)offset);
    }
    return ok;
}

static bool io_remove(void *ctx, uint32_t seg)
{
    (void)ctx;
    char p[40];
    seg_path(p, sizeof(p), seg);
    return unlink(p) == 0 || errno == ENOENT;
}

static int32_t io_size(void *ctx, uint32_t seg)
{
    (void)ctx;
    char p[40];
    seg_path(p, sizeof(p), seg);
    struct stat st;
    return stat(p, &st) == 0 ? (int32_t)st.st_size : -1;
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
#if CONFIG_GW_SF_SPILL_SDMMC
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = gw_bsp.sd.clk;
    slot.cmd = gw_bsp.sd.cmd;
    slot.d0 = gw_bsp.sd.d0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,   /* never erase a card that holds something else */
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card);
    if (err != ESP_OK) {
        snprintf(why, why_cap, "microSD not mounted (%s): no card, or not FAT",
                 esp_err_to_name(err));
        return false;
    }
    snprintf(s_dir, sizeof(s_dir), "/sdcard/sf");
    snprintf(where, where_cap, "microSD %s, %llu MB", card->cid.name,
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
#elif CONFIG_GW_SF_SPILL_LITTLEFS
    /* QEMU build: the LittleFS partition (already mounted by gw_storage) stands in */
    snprintf(s_dir, sizeof(s_dir), "/cfg/sf");
    snprintf(where, where_cap, "LittleFS /cfg/sf (stand-in for the microSD card)");
#else
    (void)where;
    (void)where_cap;
    snprintf(why, why_cap, "no microSD support in this build (CONFIG_GW_SF_SPILL_NONE)");
    return false;
#endif
#if CONFIG_GW_SF_SPILL_SDMMC || CONFIG_GW_SF_SPILL_LITTLEFS
    if (mkdir(s_dir, 0775) != 0 && errno != EEXIST) {
        snprintf(why, why_cap, "%s: mkdir failed (errno %d)", s_dir, errno);
        return false;
    }
    *io = (gw_sfspill_io_t){NULL, io_append, io_read, io_remove, io_size, io_list};
    return true;
#endif
}
