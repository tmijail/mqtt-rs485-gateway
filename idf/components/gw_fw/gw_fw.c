#include "gw_fw.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_bus.h"
#include "gw_ota.h"
#include "gw_storage.h"
#include "sdkconfig.h"

static const char *TAG = "fw";

#define RESTART_DELAY_MS 1500   /* the state NDATA (or the HTTP answer) goes out first */
#if CONFIG_SECURE_SIGNED_APPS
#define WHY_INVALID "checksum or signature"   /* FW-2: signed images required */
#else
#define WHY_INVALID "checksum"
#endif

static const gw_config_t *s_cfg;
static gw_fw_hooks_t s_hooks;
static bool s_enabled;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static gw_fw_status_t s_st;            /* under s_mux */
static gw_ota_trial_t s_trial;         /* under s_mux */
static esp_timer_handle_t s_trial_timer;
static esp_timer_handle_t s_restart_timer;
static atomic_bool s_busy;             /* a transfer is running (download or upload) */
static char s_url[GW_OTA_URL_MAX];
static char *s_ca_pem;                 /* boot allocation, never freed */
static char s_ca_error[160];           /* the configured CA file could not be read */
static char s_project[32];

/* upload state (web server task) */
static esp_ota_handle_t s_up;
static const esp_partition_t *s_up_part;
static size_t s_up_done;
static size_t s_up_total;
#define HEAD_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + \
                  sizeof(esp_app_desc_t))
static uint8_t s_up_head[HEAD_LEN];
static size_t s_up_head_len;
static char s_up_version[32];
static uint8_t s_up_pct;

/* ---------- QEMU: keep the other core out of flash while images are verified ----------
 * esp_image_verify() maps every segment of an image. On the S3 a mapping freezes the caches
 * instead of stalling the other core (SOC_CACHE_FREEZE_SUPPORTED): on the chip that core just
 * waits on a cache miss. Espressif's QEMU (esp_develop_9.2.2) does not model the freeze: code
 * the other core fetches from flash meanwhile faults and QEMU aborts ("Bad ram pointer",
 * reproduced with a task calling snprintf on core 1 during esp_ota_end() and
 * esp_ota_set_boot_partition()). CONFIG_GW_FW_PARK_OTHER_CORE (QEMU build only) parks the
 * other core in an IRAM loop for those calls; a task, not an ISR stall, so IPC calls (the
 * otadata write) still reach that core. */
#if CONFIG_GW_FW_PARK_OTHER_CORE
static volatile bool s_park;
static volatile bool s_parked;

static void IRAM_ATTR park_loop(void *arg)
{
    (void)arg;
    s_parked = true;
    while (s_park) {
        __asm__ volatile("waiti 0");   /* halted until the next interrupt (the tick) */
    }
    s_parked = false;
    vTaskDelete(NULL);
}

static void park(bool on)
{
    if (!on) {
        s_park = false;
        while (s_parked) {
            vTaskDelay(1);
        }
        return;
    }
    s_park = true;
    BaseType_t other = xPortGetCoreID() == 0 ? 1 : 0;
    if (xTaskCreatePinnedToCore(park_loop, "fw_park", 2048, NULL, configMAX_PRIORITIES - 2, NULL,
                                other) != pdPASS) {
        s_park = false;
        return;
    }
    while (!s_parked) {
        vTaskDelay(1);
    }
}
#else
static void park(bool on)
{
    (void)on;
}
#endif

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void set_status(gw_fw_phase_t phase, uint8_t progress, const char *detail,
                       const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void set_status(gw_fw_phase_t phase, uint8_t progress, const char *detail,
                       const char *fmt, ...)
{
    char text[sizeof(s_st.text)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&s_mux);
    s_st.phase = phase;
    s_st.progress = progress;
    memcpy(s_st.text, text, sizeof(text));
    snprintf(s_st.detail, sizeof(s_st.detail), "%s", detail != NULL ? detail : text);
    s_st.gen++;
    portEXIT_CRITICAL(&s_mux);
    if (s_hooks.changed != NULL) {
        s_hooks.changed();
    }
}

static void set_bytes(uint32_t bytes, uint32_t total)
{
    portENTER_CRITICAL(&s_mux);
    s_st.bytes = bytes;
    s_st.total = total;
    portEXIT_CRITICAL(&s_mux);
}

void gw_fw_get_status(gw_fw_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    out->trial_left_s = (gw_ota_trial_left_ms(&s_trial, now_ms()) + 999u) / 1000u;
    portEXIT_CRITICAL(&s_mux);
}

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The transfer failed: the running image stays, nothing changes at boot. */
static void fail(const char *fmt, ...)
{
    char why[sizeof(s_st.detail)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "firmware update failed: %s", why);
    set_status(GW_FW_FAILED, s_st.progress, why, "failed: %s", why);
    atomic_store(&s_busy, false);
}

static void on_restart(void *arg)
{
    (void)arg;
    s_hooks.restart();
}

/* The new image is written, verified and set to boot: restart into it. */
static void finished(const char *version)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(s_st.other, sizeof(s_st.other), "%s", version);
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "firmware %s written and verified; restarting into it (on trial until its "
                  "first Sparkplug session, at most %d s)",
             version, CONFIG_GW_FW_CONFIRM_TIMEOUT_S);
    set_status(GW_FW_REBOOTING, 100, NULL, "rebooting into %s", version);
    esp_timer_start_once(s_restart_timer, RESTART_DELAY_MS * 1000ULL);
}

/* ---------- trial of a new image (FW-1 health criterion, gw_ota policy) ---------- */

static void act(gw_ota_action_t a)
{
    if (a == GW_OTA_KEEP) {
        return;
    }
    char why[96];
    portENTER_CRITICAL(&s_mux);
    snprintf(why, sizeof(why), "%s", s_trial.why);
    portEXIT_CRITICAL(&s_mux);
    esp_timer_stop(s_trial_timer);
    if (a == GW_OTA_CONFIRM) {
        park(true);
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        park(false);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "firmware %s: cannot mark it valid (%s)", s_st.running, esp_err_to_name(e));
            return;
        }
        char detail[sizeof(s_st.detail)];
        snprintf(detail, sizeof(detail), "firmware %s confirmed: %s", s_st.running, why);
        ESP_LOGW(TAG, "%s; the bootloader keeps it", detail);
        set_status(GW_FW_CONFIRMED, 100, detail, "confirmed: %s", s_st.running);
        return;
    }
    char detail[sizeof(s_st.detail)];
    snprintf(detail, sizeof(detail), "firmware %s rejected (%s): back to %s", s_st.running, why,
             s_st.other[0] ? s_st.other : "the previous image");
    ESP_LOGE(TAG, "%s", detail);
    set_status(GW_FW_FAILED, 0, detail, "rolling back: %s", s_st.running);
    gw_bus_quiesce(1000);   /* no Modbus frame cut by the restart (no-op without the bus) */
    park(true);
    esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();   /* returns only on error */
    park(false);
    ESP_LOGE(TAG, "rollback impossible (%s): the new firmware stays", esp_err_to_name(e));
}

static void on_trial_timeout(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    gw_ota_action_t a = gw_ota_trial_tick(&s_trial, now_ms());
    portEXIT_CRITICAL(&s_mux);
    act(a);
}

void gw_fw_session_up(void)
{
    portENTER_CRITICAL(&s_mux);
    gw_ota_action_t a = gw_ota_trial_session_up(&s_trial);
    portEXIT_CRITICAL(&s_mux);
    act(a);
}

void gw_fw_config_rejected(void)
{
    portENTER_CRITICAL(&s_mux);
    gw_ota_action_t a = gw_ota_trial_config_rejected(&s_trial);
    portEXIT_CRITICAL(&s_mux);
    act(a);
}

void gw_fw_commissioning(void)
{
    portENTER_CRITICAL(&s_mux);
    gw_ota_action_t a = gw_ota_trial_commissioning(&s_trial);
    portEXIT_CRITICAL(&s_mux);
    act(a);
}

/* ---------- download (its own task) ---------- */

static bool take(char *why, size_t cap)
{
    if (!s_enabled) {
        snprintf(why, cap, "firmware updates are disabled (ota.enabled)");
        return false;
    }
    bool trial;
    portENTER_CRITICAL(&s_mux);
    trial = s_trial.state == GW_OTA_TRIAL_PENDING;
    portEXIT_CRITICAL(&s_mux);
    if (trial) {
        snprintf(why, cap, "the running firmware is on trial (not confirmed yet)");
        return false;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        snprintf(why, cap, "an update is already running");
        return false;
    }
    return true;
}

static void download_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t hc = {
        .url = s_url,
        .timeout_ms = CONFIG_GW_FW_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .buffer_size = 4096,
    };
    if (s_ca_pem != NULL) {
        hc.cert_pem = s_ca_pem;
    } else {
        hc.crt_bundle_attach = esp_crt_bundle_attach;
    }
    const esp_https_ota_config_t oc = {.http_config = &hc};
    esp_https_ota_handle_t h = NULL;
    esp_err_t e = esp_https_ota_begin(&oc, &h);
    if (e != ESP_OK) {
        fail("cannot download %s (%s)", s_url, esp_err_to_name(e));
        vTaskDelete(NULL);
    }
    esp_app_desc_t d;
    e = esp_https_ota_get_img_desc(h, &d);
    if (e != ESP_OK || strncmp(d.project_name, s_project, sizeof(d.project_name)) != 0) {
        esp_https_ota_abort(h);
        if (e != ESP_OK) {
            fail("not an application image (%s)", esp_err_to_name(e));
        } else {
            fail("image of '%.32s', not of '%s'", d.project_name, s_project);
        }
        vTaskDelete(NULL);
    }
    char version[sizeof(d.version) + 1];
    snprintf(version, sizeof(version), "%.32s", d.version);
    int size = esp_https_ota_get_image_size(h);
    uint32_t total = size > 0 ? (uint32_t)size : 0;
    ESP_LOGW(TAG, "downloading firmware %s (%lu bytes) from %s", version, (unsigned long)total,
             s_url);
    gw_ota_progress_t p;
    gw_ota_progress_init(&p, total, now_ms());
    uint8_t pct;
    while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        uint32_t done = (uint32_t)esp_https_ota_get_image_len_read(h);
        set_bytes(done, total);
        if (gw_ota_progress_update(&p, done, now_ms(), &pct)) {
            set_status(GW_FW_DOWNLOADING, pct, NULL, "downloading %s", version);
        }
        vTaskDelay(1);   /* the idle task of this core is watched by the TWDT */
    }
    if (e == ESP_OK && !esp_https_ota_is_complete_data_received(h)) {
        e = ESP_ERR_INVALID_SIZE;
    }
    if (e != ESP_OK) {
        esp_https_ota_abort(h);
        fail("download of %s interrupted after %lu bytes (%s)", version,
             (unsigned long)esp_https_ota_get_image_len_read(h), esp_err_to_name(e));
        vTaskDelete(NULL);
    }
    park(true);
    e = esp_https_ota_finish(h);   /* verifies the image and sets it to boot */
    park(false);
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
        fail("image %s rejected (%s)", version, WHY_INVALID);
    } else if (e != ESP_OK) {
        fail("image %s not installed (%s)", version, esp_err_to_name(e));
    } else {
        set_bytes(total ? total : p.done, total);
        finished(version);
    }
    vTaskDelete(NULL);
}

esp_err_t gw_fw_start_url(const char *url, char *why, size_t cap)
{
    char reason[96];
    if (!gw_ota_url_ok(url, false, reason, sizeof(reason))) {
        snprintf(why, cap, "%s", reason);
        ESP_LOGW(TAG, "firmware update refused: %s", why);
        set_status(GW_FW_FAILED, 0, why, "refused: %s", why);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ca_error[0] != '\0') {
        snprintf(why, cap, "%s", s_ca_error);
        set_status(GW_FW_FAILED, 0, why, "refused: CA file unavailable");
        return ESP_ERR_INVALID_STATE;
    }
    if (!take(why, cap)) {
        ESP_LOGW(TAG, "firmware update refused: %s", why);
        set_status(GW_FW_FAILED, 0, why, "refused: %s", why);
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_url, sizeof(s_url), "%s", url);
    set_bytes(0, 0);
    set_status(GW_FW_DOWNLOADING, 0, NULL, "connecting");
    if (xTaskCreatePinnedToCore(download_task, "fw_update", CONFIG_GW_FW_TASK_STACK, NULL,
                                CONFIG_GW_FW_TASK_PRIORITY, NULL, 0) != pdPASS) {
        fail("no memory for the update task");
        snprintf(why, cap, "no memory for the update task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------- upload (web server task) ---------- */

esp_err_t gw_fw_upload_begin(size_t size, char *why, size_t cap)
{
    if (!take(why, cap)) {
        return ESP_ERR_INVALID_STATE;
    }
    s_up_part = esp_ota_get_next_update_partition(NULL);
    if (s_up_part == NULL) {
        atomic_store(&s_busy, false);
        snprintf(why, cap, "no OTA partition to write");
        return ESP_ERR_NOT_FOUND;
    }
    if (size > s_up_part->size) {
        atomic_store(&s_busy, false);
        snprintf(why, cap, "image of %u bytes, the slot holds %lu", (unsigned)size,
                 (unsigned long)s_up_part->size);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t e = esp_ota_begin(s_up_part, OTA_WITH_SEQUENTIAL_WRITES, &s_up);
    if (e != ESP_OK) {
        atomic_store(&s_busy, false);
        snprintf(why, cap, "cannot write %s (%s)", s_up_part->label, esp_err_to_name(e));
        return e;
    }
    s_up_done = 0;
    s_up_total = size;
    s_up_head_len = 0;
    s_up_version[0] = '\0';
    s_up_pct = 0;
    set_bytes(0, (uint32_t)size);
    ESP_LOGW(TAG, "receiving a firmware upload (%u bytes) into %s", (unsigned)size,
             s_up_part->label);
    set_status(GW_FW_DOWNLOADING, 0, NULL, "receiving upload");
    return ESP_OK;
}

static esp_err_t up_fail(char *why, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static esp_err_t up_fail(char *why, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
    esp_ota_abort(s_up);
    fail("%s", why);
    return ESP_FAIL;
}

/* The image starts with this project's app description (checked before anything is
 * written past it, like esp_https_ota does). */
static esp_err_t check_head(char *why, size_t cap)
{
    const esp_image_header_t *h = (const esp_image_header_t *)s_up_head;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(s_up_head + sizeof(esp_image_header_t) +
                                                       sizeof(esp_image_segment_header_t));
    if (h->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return up_fail(why, cap, "not an application image");
    }
    if (strncmp(d->project_name, s_project, sizeof(d->project_name)) != 0) {
        return up_fail(why, cap, "image of '%.32s', not of '%s'", d->project_name, s_project);
    }
    snprintf(s_up_version, sizeof(s_up_version), "%.31s", d->version);
    esp_err_t e = esp_ota_write(s_up, s_up_head, s_up_head_len);
    return e == ESP_OK ? ESP_OK : up_fail(why, cap, "flash write failed (%s)", esp_err_to_name(e));
}

esp_err_t gw_fw_upload_write(const void *data, size_t len, char *why, size_t cap)
{
    const uint8_t *p = data;
    if (s_up_head_len < HEAD_LEN) {
        size_t n = HEAD_LEN - s_up_head_len < len ? HEAD_LEN - s_up_head_len : len;
        memcpy(s_up_head + s_up_head_len, p, n);
        s_up_head_len += n;
        p += n;
        len -= n;
        s_up_done += n;
        if (s_up_head_len == HEAD_LEN && check_head(why, cap) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (len > 0) {
        esp_err_t e = esp_ota_write(s_up, p, len);
        if (e != ESP_OK) {
            return up_fail(why, cap, "flash write failed after %u bytes (%s)", (unsigned)s_up_done,
                           esp_err_to_name(e));
        }
        s_up_done += len;
    }
    set_bytes((uint32_t)s_up_done, (uint32_t)s_up_total);
    uint8_t pct = s_up_total ? (uint8_t)((uint64_t)s_up_done * 100u / s_up_total) : 0;
    if (pct / 10 != s_up_pct / 10) {
        s_up_pct = pct;
        set_status(GW_FW_DOWNLOADING, pct, NULL, "receiving %s", s_up_version);
    }
    return ESP_OK;
}

esp_err_t gw_fw_upload_end(bool complete, char *why, size_t cap)
{
    if (!complete || s_up_head_len < HEAD_LEN) {
        snprintf(why, cap, complete ? "image too short" : "upload interrupted after %u bytes",
                 (unsigned)s_up_done);
        esp_ota_abort(s_up);
        fail("%s", why);
        return ESP_FAIL;
    }
    park(true);
    esp_err_t e = esp_ota_end(s_up);   /* verifies the image */
    bool verified = e == ESP_OK;
    if (verified) {
        e = esp_ota_set_boot_partition(s_up_part);
    }
    park(false);
    if (!verified && e == ESP_ERR_OTA_VALIDATE_FAILED) {
        snprintf(why, cap, "image %s rejected (%s)", s_up_version, WHY_INVALID);
    } else if (!verified) {
        snprintf(why, cap, "image %s not installed (%s)", s_up_version, esp_err_to_name(e));
    } else if (e != ESP_OK) {
        snprintf(why, cap, "cannot boot %s (%s)", s_up_part->label, esp_err_to_name(e));
    }
    if (e != ESP_OK) {
        fail("%s", why);
        return e;
    }
    finished(s_up_version);
    return ESP_OK;
}

/* ---------- boot ---------- */

esp_err_t gw_fw_init(const gw_config_t *cfg, bool enabled, const gw_fw_hooks_t *hooks)
{
    s_cfg = cfg;
    s_hooks = *hooks;
    s_enabled = enabled;
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(s_project, sizeof(s_project), "%s", app->project_name);
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (run == NULL || esp_ota_get_state_partition(run, &state) != ESP_OK) {
        state = ESP_OTA_IMG_UNDEFINED;   /* factory or flashed by serial: never on trial */
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t od;
    snprintf(s_st.running, sizeof(s_st.running), "%s", app->version);
    snprintf(s_st.slot, sizeof(s_st.slot), "%s", run != NULL ? run->label : "?");
    park(true);
    bool have_other = other != NULL && esp_ota_get_partition_description(other, &od) == ESP_OK;
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t bd;
    bool have_bad = bad != NULL && esp_ota_get_partition_description(bad, &bd) == ESP_OK;
    park(false);
    if (have_other) {
        snprintf(s_st.other, sizeof(s_st.other), "%.31s", od.version);
    }
    s_st.enabled = enabled;

    const esp_timer_create_args_t tt = {.callback = on_trial_timeout, .name = "fw_trial"};
    const esp_timer_create_args_t rt = {.callback = on_restart, .name = "fw_restart"};
    ESP_ERROR_CHECK(esp_timer_create(&tt, &s_trial_timer));
    ESP_ERROR_CHECK(esp_timer_create(&rt, &s_restart_timer));
    bool pending = state == ESP_OTA_IMG_PENDING_VERIFY;
    gw_ota_trial_init(&s_trial, pending, now_ms(), CONFIG_GW_FW_CONFIRM_TIMEOUT_S * 1000u);
    if (pending) {
        ESP_LOGW(TAG, "firmware %s (%s) on trial: confirmed by its first Sparkplug session "
                      "within %d s, otherwise back to %s",
                 s_st.running, s_st.slot, CONFIG_GW_FW_CONFIRM_TIMEOUT_S,
                 s_st.other[0] ? s_st.other : "the previous image");
        set_status(GW_FW_TRIAL, 100, NULL, "on trial: %s", s_st.running);
        esp_timer_start_once(s_trial_timer, CONFIG_GW_FW_CONFIRM_TIMEOUT_S * 1000000ULL);
    } else {
        if (have_bad) {
            char detail[sizeof(s_st.detail)];
            snprintf(detail, sizeof(detail), "firmware %.31s (%s) was rejected or did not confirm "
                     "itself; running %s again", bd.version, bad->label, s_st.running);
            ESP_LOGW(TAG, "%s", detail);
            set_status(GW_FW_ROLLED_BACK, 0, detail, "rolled back from %.31s", bd.version);
        } else {
            set_status(GW_FW_IDLE, 0, NULL, "idle");
        }
    }

    if (enabled) {
        const char *ca = cfg->ota.ca_file[0] ? cfg->ota.ca_file
                         : cfg->mqtt.tls.enabled && cfg->mqtt.tls.ca_file[0] ? cfg->mqtt.tls.ca_file
                                                                              : NULL;
        if (ca != NULL) {
            size_t len;
            char why[120];
            s_ca_pem = gw_storage_read_file(ca, &len, why, sizeof(why));
            if (s_ca_pem == NULL) {
                snprintf(s_ca_error, sizeof(s_ca_error), "%s: %s",
                         cfg->ota.ca_file[0] ? "ota.ca_file" : "mqtt.tls.ca_file", why);
                ESP_LOGE(TAG, "%s: updates by URL refused", s_ca_error);
            }
        }
        ESP_LOGI(TAG, "firmware %s in %s; updates by URL trust %s%s", s_st.running, s_st.slot,
                 s_ca_pem != NULL ? "/cfg/" : "the public root CAs",
                 s_ca_pem != NULL ? ca : "");
    } else {
        ESP_LOGI(TAG, "firmware %s in %s; updates disabled (ota.enabled)", s_st.running, s_st.slot);
    }
    return ESP_OK;
}
