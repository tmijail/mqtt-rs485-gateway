#include "gw_storage.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";

#define CFG_MOUNT   "/cfg"
#define CFG_PATH    CFG_MOUNT "/config.json"
#define CFG_MAX     CONFIG_GW_CONFIG_MAX_BYTES
#define NVS_NS      "spb"
#define NVS_BDSEQ   "bdseq"

extern const char default_config_json_start[] asm("_binary_default_config_json_start");
extern const char default_config_json_end[] asm("_binary_default_config_json_end");

static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static uint8_t s_bdseq_ram;  /* fallback if NVS is unusable */

esp_err_t gw_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    }
    s_nvs_ok = err == ESP_OK;
    if (!s_nvs_ok) {
        ESP_LOGE(TAG, "NVS unavailable (%s): bdSeq will not survive a reboot", esp_err_to_name(err));
    }
    return err;
}

uint8_t gw_storage_next_bdseq(void)
{
    uint8_t v = s_bdseq_ram;
    if (s_nvs_ok) {
        if (nvs_get_u8(s_nvs, NVS_BDSEQ, &v) == ESP_ERR_NVS_NOT_FOUND) {
            v = 0;
        }
        esp_err_t err = nvs_set_u8(s_nvs, NVS_BDSEQ, (uint8_t)(v + 1));
        if (err == ESP_OK) {
            err = nvs_commit(s_nvs);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bdSeq not persisted: %s", esp_err_to_name(err));
        }
    }
    s_bdseq_ram = (uint8_t)(v + 1);
    return v;
}

/* Boot-time buffers (the file and the parser's document tree): PSRAM when the
 * board has it, internal RAM otherwise (QEMU). Freed before anything else runs. */
static void *boot_alloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

static bool reject(gw_cfg_err_t *err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool reject(gw_cfg_err_t *err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    va_end(ap);
    return false;
}

/* Parse the override file, which exists: any problem reading it is an error. */
static bool load_file(const struct stat *st, gw_config_t *cfg, gw_cfg_err_t *err)
{
    if (st->st_size <= 0) {
        return reject(err, CFG_PATH ": archivo vacío");
    }
    if (st->st_size > CFG_MAX) {
        return reject(err, CFG_PATH ": %ld bytes; el máximo es %d (CONFIG_GW_CONFIG_MAX_BYTES)",
                      (long)st->st_size, CFG_MAX);
    }
    size_t size = (size_t)st->st_size;
    char *buf = boot_alloc(size);
    if (buf == NULL) {
        return reject(err, CFG_PATH ": sin memoria para leer %zu bytes", size);
    }
    FILE *f = fopen(CFG_PATH, "rb");
    size_t got = f != NULL ? fread(buf, 1, size, f) : 0;
    int read_errno = errno;
    if (f != NULL) {
        fclose(f);
    }
    bool ok = got == size ? gw_config_parse(buf, size, cfg, err)
                          : reject(err, CFG_PATH ": error de lectura (%zu de %zu bytes, errno %d)",
                                   got, size, read_errno);
    heap_caps_free(buf);
    return ok;
}

bool gw_storage_load_config(gw_config_t *cfg, gw_cfg_err_t *err, const char **source)
{
    *source = CFG_PATH " (LittleFS)";
    gw_config_set_allocator(boot_alloc, heap_caps_free);

    const esp_vfs_littlefs_conf_t conf = {
        .base_path = CFG_MOUNT,
        .partition_label = "storage",
        .format_if_mount_failed = false,
    };
    esp_err_t mount = esp_vfs_littlefs_register(&conf);
    if (mount != ESP_OK) {
        return reject(err, "partición LittleFS 'storage' no montable (%s)", esp_err_to_name(mount));
    }

    struct stat st;
    if (stat(CFG_PATH, &st) == 0) {
        return load_file(&st, cfg, err);
    }
    if (errno != ENOENT) {
        return reject(err, CFG_PATH ": no se puede consultar (errno %d)", errno);
    }

    /* Only a missing file selects the embedded default (CFG-2). */
    *source = "embedded default (no " CFG_PATH ")";
    /* EMBED_TXTFILES appends a NUL; strnlen excludes it. */
    size_t n = strnlen(default_config_json_start,
                       (size_t)(default_config_json_end - default_config_json_start));
    return gw_config_parse(default_config_json_start, n, cfg, err);
}
