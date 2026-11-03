#include "gw_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";

#define CFG_MOUNT   "/cfg"
#define CFG_PATH    CFG_MOUNT "/config.json"
#define CFG_MAX     (32 * 1024)
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

/* Returns a malloc'ed NUL-terminated copy of the override file, or NULL if absent. */
static char *read_override(size_t *len)
{
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = CFG_MOUNT,
        .partition_label = "storage",
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS 'storage' not mounted (%s)", esp_err_to_name(err));
        return NULL;
    }
    struct stat st;
    if (stat(CFG_PATH, &st) != 0) {
        return NULL;
    }
    if (st.st_size <= 0 || st.st_size > CFG_MAX) {
        ESP_LOGE(TAG, "%s: size %ld outside 1..%d bytes", CFG_PATH, (long)st.st_size, CFG_MAX);
        return NULL;
    }
    FILE *f = fopen(CFG_PATH, "rb");
    char *buf = f ? malloc((size_t)st.st_size + 1) : NULL;
    *len = (f && buf) ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    if (f) {
        fclose(f);
    }
    if (buf == NULL || *len != (size_t)st.st_size) {
        ESP_LOGE(TAG, "%s: read failed", CFG_PATH);
        free(buf);
        return NULL;
    }
    buf[*len] = '\0';
    return buf;
}

bool gw_storage_load_config(gw_config_t *cfg, gw_cfg_err_t *err, const char **source)
{
    size_t len = 0;
    char *file = read_override(&len);
    bool ok;
    if (file != NULL) {
        *source = CFG_PATH " (LittleFS)";
        ok = gw_config_parse(file, len, cfg, err);
        free(file);
    } else {
        *source = "embedded default";
        /* EMBED_TXTFILES appends a NUL; strnlen excludes it. */
        size_t n = strnlen(default_config_json_start,
                           (size_t)(default_config_json_end - default_config_json_start));
        ok = gw_config_parse(default_config_json_start, n, cfg, err);
    }
    return ok;
}
