#include "gw_fw.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
static SemaphoreHandle_t s_fmt_lock;   /* s_st.text and s_st.detail (formatted in place) */
static StaticSemaphore_t s_fmt_lock_buf;
/* Plain data in RTC FAST memory (8 KB, both cores, otherwise only a little-used heap region):
 * the internal DRAM heap keeps these bytes (issue #80: no PSRAM, the configuration validation
 * needs its large blocks). Locks stay in DRAM (atomic instructions). */
static RTC_FAST_ATTR gw_fw_status_t s_st;   /* under s_mux; texts under s_fmt_lock */
static RTC_FAST_ATTR gw_ota_trial_t s_trial;   /* under s_mux */
static esp_timer_handle_t s_trial_timer;
static esp_timer_handle_t s_restart_timer;
static atomic_bool s_busy;             /* a transfer is running (download or upload) */
static char *s_url;                    /* allocated for a download only (#80) */
static char *s_ca_pem;                 /* read for a download only (#80), freed after it */
static const char *s_ca_name;          /* CA file of the update server, NULL: public CAs */
static const char *s_ca_key;           /* its configuration key, for the messages */

/* upload state (web server task) */
static esp_ota_handle_t s_up;
static const esp_partition_t *s_up_part;
static size_t s_up_done;
static size_t s_up_total;
#define HEAD_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + \
                  sizeof(esp_app_desc_t))
static uint8_t *s_up_head;             /* until the head is checked (#80: not in .bss) */
static size_t s_up_head_len;
static RTC_FAST_ATTR char s_up_version[32];
static uint8_t s_up_pct;

/* ---------- QEMU: keep the other core out of flash while images are verified ----------
 * esp_image_verify() maps every segment of an image. On the S3 a mapping freezes the caches
 * instead of stalling the other core (SOC_CACHE_FREEZE_SUPPORTED): on the chip that core just
 * waits on a cache miss. Espressif's QEMU (esp_develop_9.2.2) does not model the freeze: code
 * the other core fetches from flash meanwhile faults and QEMU aborts ("Bad ram pointer",
 * reproduced with a task calling snprintf on core 1 during esp_ota_end() and
 * esp_ota_set_boot_partition()). esp_ota_begin() maps otadata too (the running image's rollback
 * state): core 1 then faulted on a peripheral write (LoadStorePIFAddrError in
 * esp_crosscore_int_send, from the syslog task) at the start of a download.
 * CONFIG_GW_FW_PARK_OTHER_CORE (QEMU build only) parks the other core in an IRAM loop for those
 * calls; a task, not an ISR stall, so IPC calls (the otadata write) still reach that core. */
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
/* The parked core must be core 1: core 0's tick interrupt also runs the interrupt watchdog's
 * hook, which writes its timer group (TIMG1) and faulted in QEMU during the freeze
 * (LoadStorePIFAddrError in reconfigure_ticks, seen with an upload served by httpd on core 1);
 * core 1's hook only sets a flag. So @p fn runs on core 0: here, or in a task pinned there. */
typedef struct {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
} core0_call_t;

static void core0_task(void *arg)
{
    core0_call_t *c = arg;
    c->fn(c->arg);
    xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

static void on_core0(void (*fn)(void *), void *arg)
{
    if (xPortGetCoreID() == 0) {
        fn(arg);
        return;
    }
    StaticSemaphore_t buf;
    core0_call_t c = {.fn = fn, .arg = arg, .done = xSemaphoreCreateBinaryStatic(&buf)};
    if (xTaskCreatePinnedToCore(core0_task, "fw_install", 6144, &c, CONFIG_GW_FW_TASK_PRIORITY,
                                NULL, 0) != pdPASS) {
        fn(arg);   /* no memory for the task: as on the chip, without the QEMU precaution */
        return;
    }
    xSemaphoreTake(c.done, portMAX_DELAY);
}
#else
static void park(bool on)
{
    (void)on;
}

static void on_core0(void (*fn)(void *), void *arg)
{
    fn(arg);
}
#endif

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* s_st's texts. The mutex exists from gw_fw_early_init(), before the shell starts (issue #101:
 * a 'status' typed during the boot raced gw_fw_init(), which created it). A caller that did not
 * take it (nothing created it yet) does not give it either. */
static bool text_lock(void)
{
    SemaphoreHandle_t m = s_fmt_lock;
    return m != NULL && xSemaphoreTake(m, portMAX_DELAY) == pdTRUE;
}

static void text_unlock(bool taken)
{
    if (taken) {
        xSemaphoreGive(s_fmt_lock);
    }
}

/* Formatted in place under the mutex, which the readers take too: no copy buffers (on the stack
 * of the callers, among them the esp_timer task, nor in .bss: issue #80). @p release: the
 * transfer ends with this state; the busy flag drops in the same critical section as the phase,
 * so nobody sees the new state with the old flag (issue #101: a new update could start in
 * between and have its "connecting" overwritten by this one's "failed"). */
static void update_status(gw_fw_phase_t phase, uint8_t progress, const char *detail, bool release,
                          const char *fmt, va_list ap)
{
    bool taken = text_lock();
    if ((size_t)vsnprintf(s_st.text, sizeof(s_st.text), fmt, ap) >= sizeof(s_st.text)) {
        ESP_LOGW(TAG, "update state cut to %u characters", (unsigned)(sizeof(s_st.text) - 1));
    }
    if (detail != s_st.detail) {
        snprintf(s_st.detail, sizeof(s_st.detail), "%s", detail != NULL ? detail : s_st.text);
    }
    portENTER_CRITICAL(&s_mux);
    s_st.phase = phase;
    s_st.progress = progress;
    s_st.gen++;
    if (release) {
        atomic_store(&s_busy, false);
    }
    portEXIT_CRITICAL(&s_mux);
    text_unlock(taken);
    if (s_hooks.changed != NULL) {
        s_hooks.changed();
    }
}

static void set_status(gw_fw_phase_t phase, uint8_t progress, const char *detail,
                       const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void set_status(gw_fw_phase_t phase, uint8_t progress, const char *detail,
                       const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    update_status(phase, progress, detail, false, fmt, ap);
    va_end(ap);
}

static void set_status_release(gw_fw_phase_t phase, uint8_t progress, const char *detail,
                               const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void set_status_release(gw_fw_phase_t phase, uint8_t progress, const char *detail,
                               const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    update_status(phase, progress, detail, true, fmt, ap);
    va_end(ap);
}

static void set_bytes(uint32_t bytes, uint32_t total)
{
    portENTER_CRITICAL(&s_mux);
    s_st.bytes = bytes;
    s_st.total = total;
    portEXIT_CRITICAL(&s_mux);
}

void gw_fw_get_state(char *text, size_t cap, uint8_t *progress)
{
    bool taken = text_lock();
    portENTER_CRITICAL(&s_mux);
    size_t n = strnlen(s_st.text, sizeof(s_st.text));
    n = n < cap ? n : cap - 1;
    memcpy(text, s_st.text, n);
    text[n] = '\0';
    *progress = s_st.progress;
    portEXIT_CRITICAL(&s_mux);
    text_unlock(taken);
}

void gw_fw_get_status(gw_fw_status_t *out)
{
    bool taken = text_lock();
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    out->trial_left_s = (gw_ota_trial_left_ms(&s_trial, now_ms()) + 999u) / 1000u;
    portEXIT_CRITICAL(&s_mux);
    text_unlock(taken);
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
    /* whoever sees the failed state may start the next update right away: the flag drops with
     * the phase, under the same lock (issue #101) */
    set_status_release(GW_FW_FAILED, s_st.progress, why, "failed: %s", why);
}

/* Issue #101: a refused request is reported without hiding a running transfer's state or the
 * trial of a new image (its phase, and with it the time left, stay). */
static void refused(const char *why)
{
    ESP_LOGW(TAG, "firmware update refused: %s", why);
    portENTER_CRITICAL(&s_mux);
    gw_ota_refusal_t r = gw_ota_refusal(&s_trial, atomic_load(&s_busy));
    portEXIT_CRITICAL(&s_mux);
    if (r == GW_OTA_REFUSAL_IN_TRIAL) {
        char detail[sizeof(s_st.detail)];
        snprintf(detail, sizeof(detail), "refused: %s; firmware %s on trial", why, s_st.running);
        set_status(GW_FW_TRIAL, 100, detail, "refused: %s", why);
    } else if (r == GW_OTA_REFUSAL_REPORT) {
        set_status(GW_FW_FAILED, 0, why, "refused: %s", why);
    }
}

static void on_restart(void *arg)
{
    (void)arg;
    s_hooks.restart();
}

/* An orderly restart in RESTART_DELAY_MS: the state NDATA (or the HTTP answer) goes out first. */
static void restart_soon(void)
{
    if (s_restart_timer == NULL) {   /* created when needed: no allocation in a normal boot */
        const esp_timer_create_args_t rt = {.callback = on_restart, .name = "fw_restart"};
        if (esp_timer_create(&rt, &s_restart_timer) != ESP_OK) {
            ESP_LOGE(TAG, "no memory for the restart timer: restarting now");
            s_hooks.restart();
            return;
        }
    }
    esp_timer_start_once(s_restart_timer, RESTART_DELAY_MS * 1000ULL);
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
    restart_soon();
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
    if (s_trial_timer != NULL) {
        esp_timer_stop(s_trial_timer);
    }
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
        if (gw_storage_nvs_encryption_pending()) {
            /* issue #94: the trial ran on the unencrypted NVS (a rollback had to find it as
             * the previous image left it); now no rollback can come, and the next boot
             * moves it into the encrypted NVS */
            ESP_LOGW(TAG, "firmware %s confirmed: restarting once to encrypt the NVS",
                     s_st.running);
            restart_soon();
        }
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
    trial = gw_ota_trial_open(&s_trial);
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

static const char *project(void)
{
    return esp_app_get_description()->project_name;
}

/* Buffers of a transfer: PSRAM when there is some, internal RAM otherwise; freed after it. */
static void *xfer_alloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

static void download_release(void)
{
    gw_storage_free(s_ca_pem);
    s_ca_pem = NULL;
    heap_caps_free(s_url);
    s_url = NULL;
}

static void download_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* A failed download ends its task. The URL and the CA copy go back to the heap first: the
 * failed state lets the next update start at once, and it allocates its own. */
static void download_fail(const char *fmt, ...)
{
    char why[sizeof(s_st.detail)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    download_release();
    fail("%s", why);
    vTaskDelete(NULL);
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
        download_fail("cannot download %s (%s)", s_url, esp_err_to_name(e));
    }
    esp_app_desc_t d;
    e = esp_https_ota_get_img_desc(h, &d);
    if (e != ESP_OK || strncmp(d.project_name, project(), sizeof(d.project_name)) != 0) {
        esp_https_ota_abort(h);   /* frees h: only the local copies are used below */
        h = NULL;
        if (e != ESP_OK) {
            download_fail("not an application image (%s)", esp_err_to_name(e));
        }
        download_fail("image of '%.32s', not of '%s'", d.project_name, project());
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
    /* the first call runs esp_ota_begin(), which reads otadata through a mapping (rollback
     * state of the running image): the other core is parked for it too (QEMU) */
    park(true);
    e = esp_https_ota_perform(h);
    park(false);
    for (; e == ESP_ERR_HTTPS_OTA_IN_PROGRESS; e = esp_https_ota_perform(h)) {
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
        /* issue #98: esp_https_ota_abort() frees the handle; read what it holds before */
        unsigned long got = (unsigned long)esp_https_ota_get_image_len_read(h);
        esp_https_ota_abort(h);
        h = NULL;
        download_fail("download of %s interrupted after %lu bytes (%s)", version, got,
                      esp_err_to_name(e));
    }
    park(true);
    e = esp_https_ota_finish(h);   /* verifies the image and sets it to boot; frees h always */
    h = NULL;
    park(false);
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
        download_fail("image %s rejected (%s)", version, WHY_INVALID);
    } else if (e != ESP_OK) {
        download_fail("image %s not installed (%s)", version, esp_err_to_name(e));
    }
    download_release();
    set_bytes(total ? total : p.done, total);
    finished(version);
    vTaskDelete(NULL);
}

esp_err_t gw_fw_start_url(const char *url, char *why, size_t cap)
{
    char reason[96];
    if (!gw_ota_url_ok(url, false, reason, sizeof(reason))) {
        snprintf(why, cap, "%s", reason);
        refused(why);
        return ESP_ERR_INVALID_ARG;
    }
    if (!take(why, cap)) {
        refused(why);
        return ESP_ERR_INVALID_STATE;
    }
    s_url = xfer_alloc(GW_OTA_URL_MAX);
    if (s_url == NULL) {
        snprintf(why, cap, "no memory for the update");
        ESP_LOGE(TAG, "firmware update refused: %s", why);
        set_status_release(GW_FW_FAILED, 0, why, "refused: no memory");
        return ESP_ERR_NO_MEM;
    }
    if (s_ca_name != NULL) {
        /* read now, not at boot: nothing stays allocated while no update runs (#80) */
        size_t len;
        char err[120];
        s_ca_pem = gw_storage_read_file(s_ca_name, &len, err, sizeof(err));
        if (s_ca_pem == NULL) {
            snprintf(why, cap, "%s: %s", s_ca_key, err);
            ESP_LOGE(TAG, "firmware update refused: %s", why);
            download_release();
            set_status_release(GW_FW_FAILED, 0, why, "refused: CA file unavailable");
            return ESP_ERR_INVALID_STATE;
        }
    }
    snprintf(s_url, GW_OTA_URL_MAX, "%s", url);
    set_bytes(0, 0);
    set_status(GW_FW_DOWNLOADING, 0, NULL, "connecting");
    if (xTaskCreatePinnedToCore(download_task, "fw_update", CONFIG_GW_FW_TASK_STACK, NULL,
                                CONFIG_GW_FW_TASK_PRIORITY, NULL, 0) != pdPASS) {
        download_release();
        fail("no memory for the update task");
        snprintf(why, cap, "no memory for the update task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------- upload (web server task) ---------- */

/* esp_ota_begin() reads otadata through a mapping: like an install, on core 0 with the other
 * core parked (QEMU) */
static void begin_upload(void *arg)
{
    park(true);
    *(esp_err_t *)arg = esp_ota_begin(s_up_part, OTA_WITH_SEQUENTIAL_WRITES, &s_up);
    park(false);
}

static void up_release(void)
{
    heap_caps_free(s_up_head);
    s_up_head = NULL;
}

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
    s_up_head = xfer_alloc(HEAD_LEN);
    if (s_up_head == NULL) {
        atomic_store(&s_busy, false);
        snprintf(why, cap, "no memory for the upload");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = ESP_OK;
    on_core0(begin_upload, &e);
    if (e != ESP_OK) {
        up_release();
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
    up_release();
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
    if (strncmp(d->project_name, project(), sizeof(d->project_name)) != 0) {
        return up_fail(why, cap, "image of '%.32s', not of '%s'", d->project_name, project());
    }
    snprintf(s_up_version, sizeof(s_up_version), "%.31s", d->version);
    esp_err_t e = esp_ota_write(s_up, s_up_head, s_up_head_len);
    if (e != ESP_OK) {
        return up_fail(why, cap, "flash write failed (%s)", esp_err_to_name(e));
    }
    up_release();   /* written: the rest goes straight to the flash */
    return ESP_OK;
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

/* Verify the uploaded image and set it to boot, with the other core parked (QEMU). */
typedef struct {
    esp_err_t err;
    bool verified;
} install_t;

static void install_upload(void *arg)
{
    install_t *in = arg;
    park(true);
    in->err = esp_ota_end(s_up);   /* verifies the image */
    in->verified = in->err == ESP_OK;
    if (in->verified) {
        in->err = esp_ota_set_boot_partition(s_up_part);
    }
    park(false);
}

esp_err_t gw_fw_upload_end(bool complete, char *why, size_t cap)
{
    if (!complete || s_up_head_len < HEAD_LEN) {
        snprintf(why, cap, complete ? "image too short" : "upload interrupted after %u bytes",
                 (unsigned)s_up_done);
        esp_ota_abort(s_up);
        up_release();
        fail("%s", why);
        return ESP_FAIL;
    }
    install_t in = {0};
    on_core0(install_upload, &in);
    esp_err_t e = in.err;
    bool verified = in.verified;
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

static bool s_early;

void gw_fw_early_init(void)
{
    if (s_early) {
        return;
    }
    s_early = true;
    s_fmt_lock = xSemaphoreCreateMutexStatic(&s_fmt_lock_buf);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (run == NULL || esp_ota_get_state_partition(run, &state) != ESP_OK) {
        state = ESP_OTA_IMG_UNDEFINED;   /* factory or flashed by serial: never on trial */
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t od;
    park(true);
    bool have_other = other != NULL && esp_ota_get_partition_description(other, &od) == ESP_OK;
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t bd;
    bool have_bad = bad != NULL && esp_ota_get_partition_description(bad, &bd) == ESP_OK;
    park(false);
    bool pending = state == ESP_OTA_IMG_PENDING_VERIFY;
    /* issue #101: written under the locks the readers take (the shell may already ask) */
    bool taken = text_lock();
    portENTER_CRITICAL(&s_mux);
    snprintf(s_st.running, sizeof(s_st.running), "%s", app->version);
    snprintf(s_st.slot, sizeof(s_st.slot), "%s", run != NULL ? run->label : "?");
    snprintf(s_st.other, sizeof(s_st.other), "%.31s", have_other ? od.version : "");
    gw_ota_trial_init(&s_trial, pending, now_ms(), CONFIG_GW_FW_CONFIRM_TIMEOUT_S * 1000u);
    portEXIT_CRITICAL(&s_mux);
    text_unlock(taken);

    if (pending) {
        ESP_LOGW(TAG, "firmware %s (%s) on trial: confirmed by its first Sparkplug session "
                      "within %d s, otherwise back to %s",
                 s_st.running, s_st.slot, CONFIG_GW_FW_CONFIRM_TIMEOUT_S,
                 s_st.other[0] ? s_st.other : "the previous image");
        set_status(GW_FW_TRIAL, 100, NULL, "on trial: %s", s_st.running);
    } else if (have_bad) {
        char detail[sizeof(s_st.detail)];
        snprintf(detail, sizeof(detail), "firmware %.31s (%s) was rejected or did not confirm "
                 "itself; running %s again", bd.version, bad->label, s_st.running);
        ESP_LOGW(TAG, "%s", detail);
        set_status(GW_FW_ROLLED_BACK, 0, detail, "rolled back from %.31s", bd.version);
    } else {
        set_status(GW_FW_IDLE, 0, NULL, "idle");
    }
}

esp_err_t gw_fw_init(const gw_config_t *cfg, bool enabled, const gw_fw_hooks_t *hooks)
{
    gw_fw_early_init();
    s_cfg = cfg;
    s_hooks = *hooks;
    s_enabled = enabled;
    portENTER_CRITICAL(&s_mux);
    s_st.enabled = enabled;
    bool pending = gw_ota_trial_open(&s_trial);
    uint32_t left = gw_ota_trial_left_ms(&s_trial, now_ms());
    portEXIT_CRITICAL(&s_mux);
    if (pending) {
        /* only a new image on trial needs the timer (#80: nothing allocated otherwise); the
         * trial counts from gw_fw_early_init() */
        const esp_timer_create_args_t tt = {.callback = on_trial_timeout, .name = "fw_trial"};
        ESP_ERROR_CHECK(esp_timer_create(&tt, &s_trial_timer));
        esp_timer_start_once(s_trial_timer, (left ? left : 1) * 1000ULL);
    }

    if (enabled) {
        const char *ca = cfg->ota.ca_file[0] ? cfg->ota.ca_file
                         : cfg->mqtt.tls.enabled && cfg->mqtt.tls.ca_file[0] ? cfg->mqtt.tls.ca_file
                                                                              : NULL;
        s_ca_name = ca;
        s_ca_key = cfg->ota.ca_file[0] ? "ota.ca_file" : "mqtt.tls.ca_file";
        ESP_LOGI(TAG, "firmware %s in %s; updates by URL trust %s%s", s_st.running, s_st.slot,
                 ca != NULL ? "/cfg/" : "the public root CAs", ca != NULL ? ca : "");
    } else {
        ESP_LOGI(TAG, "firmware %s in %s; updates disabled (ota.enabled)", s_st.running, s_st.slot);
    }
    return ESP_OK;
}
