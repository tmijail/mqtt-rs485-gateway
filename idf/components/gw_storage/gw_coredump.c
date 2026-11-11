#include "gw_coredump.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "coredump";

static atomic_int s_state = GW_CDUMP_NONE;   /* gw_cdump_state_t */
static atomic_uint s_size;
static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;              /* reads, the summary and the erase */
static StaticSemaphore_t s_lock_buf;

void gw_coredump_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                      NULL);
    if (s_part == NULL) {
        ESP_LOGW(TAG, "no coredump partition: a panic leaves no core dump");
        return;
    }
    size_t addr = 0, size = 0;
    esp_err_t e = esp_core_dump_image_get(&addr, &size);
    if (e == ESP_ERR_NOT_FOUND) {
        return;   /* blank: no dump */
    }
    if (e != ESP_OK) {
        /* a size word that is not blank and not possible: a dump that is not complete */
        atomic_store(&s_state, GW_CDUMP_CORRUPT);
        ESP_LOGW(TAG, "core dump partition not blank but no readable dump (%s): erase it with "
                      "DELETE /api/coredump or the shell's 'coredump erase'",
                 esp_err_to_name(e));
        return;
    }
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    bool valid = esp_core_dump_image_check() == ESP_OK;
#else
    bool valid = false;
#endif
    atomic_store(&s_size, (unsigned)size);
    atomic_store(&s_state, valid ? GW_CDUMP_VALID : GW_CDUMP_CORRUPT);
    ESP_LOGW(TAG, "core dump of %u bytes in flash (%s): get it with GET /api/coredump or the "
                  "shell's 'coredump dump', then erase it",
             (unsigned)size, valid ? "checksum correct" : "CORRUPT");
}

bool gw_coredump_present(void)
{
    return atomic_load(&s_state) != GW_CDUMP_NONE;
}

gw_cdump_state_t gw_coredump_state(uint32_t *size)
{
    gw_cdump_state_t st = (gw_cdump_state_t)atomic_load(&s_state);
    *size = atomic_load(&s_size);
    return st;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
/* A text field of the dump, which need not end with a NUL. */
static void copy_text(char *dst, size_t cap, const void *src, size_t len)
{
    size_t n = strnlen(src, len);
    n = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void copy_summary(gw_cdump_info_t *out, const esp_core_dump_summary_t *s)
{
    out->has_summary = true;
    copy_text(out->task, sizeof(out->task), s->exc_task, sizeof(s->exc_task));
    out->pc = s->exc_pc;
    uint32_t depth = s->exc_bt_info.depth;
    if (depth > GW_CDUMP_BT_MAX) {
        depth = GW_CDUMP_BT_MAX;
    }
    memcpy(out->bt, s->exc_bt_info.bt, depth * sizeof(out->bt[0]));
    out->depth = (uint8_t)depth;
    out->bt_corrupted = s->exc_bt_info.corrupted;
    out->exc_cause = s->ex_info.exc_cause;
    out->exc_vaddr = s->ex_info.exc_vaddr;
    copy_text(out->app_sha256, sizeof(out->app_sha256), s->app_elf_sha256,
              sizeof(s->app_elf_sha256));
}
#endif

esp_err_t gw_coredump_summary(gw_cdump_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = gw_coredump_state(&out->size);
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (out->state != GW_CDUMP_VALID) {
        return ESP_OK;
    }
    /* about 300 B, for this call only: from the heap (PSRAM when present), not the caller's
     * stack (the console task, the web server task) */
    esp_core_dump_summary_t *s = heap_caps_malloc_prefer(sizeof(*s), 2,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                         MALLOC_CAP_8BIT);
    if (s == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s, 0, sizeof(*s));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* the state again under the lock: an erase may have come in between */
    if (atomic_load(&s_state) == GW_CDUMP_VALID && esp_core_dump_get_summary(s) == ESP_OK) {
        copy_summary(out, s);
        if (esp_core_dump_get_panic_reason(out->reason, sizeof(out->reason)) != ESP_OK) {
            out->reason[0] = '\0';
        }
    }
    xSemaphoreGive(s_lock);
    heap_caps_free(s);
#endif
    return ESP_OK;
}

esp_err_t gw_coredump_read(uint32_t offset, void *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e;
    uint32_t size = atomic_load(&s_size);
    if (atomic_load(&s_state) == GW_CDUMP_NONE || s_part == NULL) {
        e = ESP_ERR_NOT_FOUND;
    } else if (size == 0) {
        e = ESP_ERR_INVALID_SIZE;
    } else if (offset > size || len > size - offset) {
        e = ESP_ERR_INVALID_ARG;
    } else {
        e = esp_partition_read(s_part, offset, buf, len);
    }
    xSemaphoreGive(s_lock);
    return e;
}

esp_err_t gw_coredump_erase(const char *who)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t e = esp_core_dump_image_erase();
    if (e == ESP_OK) {
        atomic_store(&s_state, GW_CDUMP_NONE);
        atomic_store(&s_size, 0u);
    }
    xSemaphoreGive(s_lock);
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "core dump erased through the %s", who);
    } else {
        ESP_LOGE(TAG, "core dump not erased: %s", esp_err_to_name(e));
    }
    return e;
}
