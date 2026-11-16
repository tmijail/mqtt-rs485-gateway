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
#include "esp_ipc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_bootloop.h"
#include "gw_bus.h"
#include "gw_storage.h"
#include "sdkconfig.h"

static const char *TAG = "fw";

#define NVS_FW_LAST "fw_last"   /* issue #144 item 5 */

/* Issues #102 and #144: what this image declares about itself, right after its esp_app_desc_t
 * (see gw_ota.h); an update reads the same block of the incoming image. GW_LFS_DISK_VERSION and
 * GW_NVS_PAGE_VERSION come from the littlefs and nvs_flash sources (CMakeLists.txt, #148). */
const __attribute__((section(".rodata_custom_desc"), used)) gw_ota_caps_t gw_app_caps = {
    .magic = GW_OTA_CAPS_MAGIC,
    .version = GW_OTA_CAPS_VERSION,
    .size = sizeof(gw_ota_caps_t),
    .flags = 0
#if CONFIG_NVS_ENCRYPTION
             | GW_OTA_CAP_NVS_ENCRYPTION
#endif
#if CONFIG_SECURE_FLASH_ENC_ENABLED
             | GW_OTA_CAP_FLASH_ENCRYPTION
#endif
    ,
    .cfg_schema = GW_CONFIG_SCHEMA,
    .nvs_format = (uint16_t)(((0xFFu - GW_NVS_PAGE_VERSION) << 8) | GW_NVS_RECORDS),
    .lfs_disk = GW_LFS_DISK_VERSION,
};
_Static_assert(sizeof(gw_ota_caps_t) == GW_OTA_CAPS_SIZE, "gw_ota_caps_t layout");
_Static_assert(GW_OTA_CAPS_OFFSET == sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                                         sizeof(esp_app_desc_t), "caps offset");

#define RESTART_DELAY_MS 1500   /* the state NDATA (or the HTTP answer) goes out first */
#define BACKSTOP_GRACE_MS 10000u   /* the timer after the backstop of the main task's check */
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
static SemaphoreHandle_t s_ota_lock;   /* otadata writes: confirm, boot partition (issue #99) */
static StaticSemaphore_t s_ota_lock_buf;
/* Plain data in RTC FAST memory (8 KB, both cores, otherwise only a little-used heap region):
 * the internal DRAM heap keeps these bytes (issue #80: internal RAM is shared with Wi-Fi, lwIP,
 * the stacks and the fault mode of #135). Locks stay in DRAM (atomic instructions). */
static RTC_FAST_ATTR gw_fw_status_t s_st;   /* under s_mux; texts under s_fmt_lock */
static RTC_FAST_ATTR gw_ota_trial_t s_trial;   /* under s_mux */
static RTC_FAST_ATTR gw_fwlast_t s_last;    /* the fw_last record (issue #144 item 5) */
/* Issue #144 item 4: the boot-loop counter, in RTC memory that a reset does not clear */
static RTC_NOINIT_ATTR gw_bootloop_t s_loop;
static uint32_t s_loop_count;
static bool s_safe;
static atomic_bool s_planned;          /* the next restart is planned by gw_fw, not the operator */
static gw_ota_mode_t s_mode;           /* the mode that this boot reached */
static gw_ota_caps_info_t s_own;       /* gw_app_caps as gw_ota_caps_parse() reads it */
static bool s_other_invalid;           /* the other slot failed its verification in this boot */
static esp_timer_handle_t s_backstop_timer;
static esp_timer_handle_t s_loop_timer;   /* the clear after 120 s, or the end of safe mode */
static esp_timer_handle_t s_restart_timer;
static atomic_bool s_busy;             /* a transfer is running (download or upload) */
static bool s_force;                   /* the running transfer was forced (issue #144 item 3) */
static bool s_restore;                 /* issue #146: restore config.prev.json before the boot */
static uint32_t s_restore_hash;        /* ... while config.json keeps this hash */
static char *s_url;                    /* allocated for a download only (#80) */
static char *s_ca_pem;                 /* read for a download only (#80), freed after it */
static const char *s_ca_name;          /* CA file of the update server, NULL: public CAs */
static const char *s_ca_key;           /* its configuration key, for the messages */

/* upload state (web server task) */
static esp_ota_handle_t s_up;
static const esp_partition_t *s_up_part;
static size_t s_up_done;
static size_t s_up_total;
#define HEAD_LEN (GW_OTA_CAPS_OFFSET + GW_OTA_CAPS_SIZE)   /* up to the capabilities (#102) */
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
 * calls; a task, not an ISR stall, so the flash driver's IPC calls (the otadata write:
 * esp_ipc_call_nonblocking(), which raises ipc1 to the top priority) still reach that core. A
 * blocking esp_ipc_call() made at a low priority would not (CONFIG_ESP_IPC_USES_CALLERS_PRIORITY
 * runs ipc1 at the caller's priority, below the park task): none is made while parked. */
#if CONFIG_GW_FW_PARK_OTHER_CORE
/* Issue #111: a park must never leave core 0 waiting on core 1. The park task used to preempt
 * whatever ran on core 1. When that was the bus task holding a lock (gettimeofday()'s
 * s_adjtime_lock, which the syslog copy of a log line also takes; or stdout's, in a log line), the
 * image verification on core 0 blocked on it for good (core 0 idle, core 1 in park_loop; seen
 * with a gdb task dump). The rules now:
 * - a task that can only run on core 1 is parked where it holds no lock. The bus task, the only
 *   task of ours pinned there, first stops between two transactions (gw_bus_hold); IDF's own
 *   (IDLE1, and ipc1, which runs above the park task) take no lock that a parked section needs.
 *   A task with no affinity that the park task preempts on core 1 is run by core 0 instead
 *   (FreeRTOS SMP scheduling), so it still releases what it holds. A new task pinned to core 1
 *   must take part like the bus task;
 * - nothing of ours between park(true) and park(false) logs; the lines around it ("parking
 *   core 1", "core 1 resumed after N ms") come before the park task starts and after it ended.
 *   IDF's own lines inside (esp_image's) go out as usual: no lock they need is left on core 1;
 * - a park lasts at most PARK_MAX_MS: past it core 1 resumes by itself (the emulator fault the
 *   park avoids may follow) and park(false) logs it, an error for the E2E. */
#define PARK_HOLD_MS 10000u   /* least wait for the bus task to stop (more with slow RS485, #112) */
#define PARK_MAX_MS 45000u    /* below the harness' 60 s without console output (issue #92) */
static volatile bool s_park;
static volatile bool s_parked;
static volatile bool s_park_overrun;   /* set by park_loop when it gave up */
static bool s_park_on;                 /* park(true) parked core 1: park(false) undoes it */
static int64_t s_park_t0;
static SemaphoreHandle_t s_park_lock;  /* one park at a time, park(true) to park(false) */
static StaticSemaphore_t s_park_lock_buf;

static void IRAM_ATTR park_loop(void *arg)
{
    (void)arg;
    const TickType_t t0 = xTaskGetTickCount();   /* in IRAM (FreeRTOS linker fragment) */
    s_parked = true;
    while (s_park) {
        if (xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(PARK_MAX_MS)) {
            s_park_overrun = true;
            break;
        }
        __asm__ volatile("waiti 0");   /* halted until the next interrupt (the tick) */
    }
    s_parked = false;
    vTaskDelete(NULL);
}

static void park_init(void)
{
    s_park_lock = xSemaphoreCreateMutexStatic(&s_park_lock_buf);
}

/* Issue #112: a blocking IPC call to the parked core waits until the park ends (ipc1 runs at
 * the caller's priority, CONFIG_ESP_IPC_USES_CALLERS_PRIORITY, below fw_park), and deadlocks if
 * the caller holds a lock the parked section needs. Nothing does it today; the build wraps both
 * calls (CMakeLists.txt, --wrap) so that one made while parked is recorded here, without a log
 * line or a lock, and reported by park(false). */
static portMUX_TYPE s_ipc_mux = portMUX_INITIALIZER_UNLOCKED;
static const char *s_ipc_api;          /* the first offending call of this park, NULL: none */
static char s_ipc_task[configMAX_TASK_NAME_LEN];
static esp_ipc_func_t s_ipc_func;

esp_err_t __real_esp_ipc_call(uint32_t cpu_id, esp_ipc_func_t func, void *arg);
esp_err_t __real_esp_ipc_call_blocking(uint32_t cpu_id, esp_ipc_func_t func, void *arg);
esp_err_t __wrap_esp_ipc_call(uint32_t cpu_id, esp_ipc_func_t func, void *arg);
esp_err_t __wrap_esp_ipc_call_blocking(uint32_t cpu_id, esp_ipc_func_t func, void *arg);

static void ipc_check(uint32_t cpu_id, const char *api, esp_ipc_func_t func)
{
    if (!s_parked || cpu_id != 1) {
        return;
    }
    const char *name = xPortInIsrContext() ? "(ISR)" : pcTaskGetName(NULL);
    portENTER_CRITICAL_SAFE(&s_ipc_mux);
    if (s_ipc_api == NULL) {
        s_ipc_api = api;
        s_ipc_func = func;
        strlcpy(s_ipc_task, name, sizeof(s_ipc_task));
    }
    portEXIT_CRITICAL_SAFE(&s_ipc_mux);
}

esp_err_t __wrap_esp_ipc_call(uint32_t cpu_id, esp_ipc_func_t func, void *arg)
{
    ipc_check(cpu_id, "esp_ipc_call", func);
    return __real_esp_ipc_call(cpu_id, func, arg);
}

esp_err_t __wrap_esp_ipc_call_blocking(uint32_t cpu_id, esp_ipc_func_t func, void *arg)
{
    ipc_check(cpu_id, "esp_ipc_call_blocking", func);
    return __real_esp_ipc_call_blocking(cpu_id, func, arg);
}

/* The offending call of the park that just ended, if any (park(false), core 1 running). */
static void ipc_report(void)
{
    portENTER_CRITICAL(&s_ipc_mux);
    const char *api = s_ipc_api;
    esp_ipc_func_t func = s_ipc_func;
    char task[sizeof(s_ipc_task)];
    memcpy(task, s_ipc_task, sizeof(task));
    s_ipc_api = NULL;
    portEXIT_CRITICAL(&s_ipc_mux);
    if (api != NULL) {
        ESP_LOGE(TAG, "blocking IPC to core 1 while it was parked: %s(%p) from task %s "
                      "(issue #112)", api, (void *)func, task);
    }
}

static void park(bool on)
{
    if (!on) {
        if (!s_park_on) {
            return;   /* that park(true) did not park */
        }
        s_park = false;
        while (s_parked) {
            vTaskDelay(1);
        }
        s_park_on = false;
        bool overrun = s_park_overrun;
        s_park_overrun = false;
        unsigned long ms = (unsigned long)((esp_timer_get_time() - s_park_t0) / 1000);
        if (overrun) {
            ESP_LOGE(TAG, "core 1 park bound reached: core 1 resumed by itself after %u s, "
                          "before the parked section ended (issue #111)", PARK_MAX_MS / 1000);
        }
        ipc_report();
        /* before the bus task goes on: none of its lines comes before this one (the E2E) */
        ESP_LOGI(TAG, "core 1 resumed after %lu ms", ms);
        gw_bus_release();
        xSemaphoreGive(s_park_lock);
        return;
    }
    xSemaphoreTake(s_park_lock, portMAX_DELAY);
    uint32_t waited_ms = PARK_HOLD_MS;
    if (!gw_bus_hold(PARK_HOLD_MS, &waited_ms)) {
        xSemaphoreGive(s_park_lock);
        /* the bound gw_bus_hold() used: longer than PARK_HOLD_MS with slow RS485 settings */
        ESP_LOGE(TAG, "core 1 park refused: the bus task did not stop within %lu ms (issue #111)",
                 (unsigned long)waited_ms);
        return;
    }
    ESP_LOGI(TAG, "parking core 1 (bus task stopped)");
    s_park = true;
    BaseType_t other = xPortGetCoreID() == 0 ? 1 : 0;
    if (xTaskCreatePinnedToCore(park_loop, "fw_park", 2048, NULL, configMAX_PRIORITIES - 2, NULL,
                                other) != pdPASS) {
        s_park = false;
        gw_bus_release();
        xSemaphoreGive(s_park_lock);
        ESP_LOGE(TAG, "core 1 park refused: no memory for its task (issue #111)");
        return;
    }
    while (!s_parked) {
        vTaskDelay(1);
    }
    s_park_t0 = esp_timer_get_time();
    s_park_on = true;
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
static void park_init(void)
{
}

static void park(bool on)
{
    (void)on;
}

static void on_core0(void (*fn)(void *), void *arg)
{
    fn(arg);
}
#endif

/* The uptime of this boot (ms, 32 bits: it wraps after 49 days, and gw_ota compares with that
 * in mind). From the FreeRTOS tick, not esp_timer: in QEMU the system timer does not restart at
 * a software reset, so esp_timer_get_time() can be minutes at the start of app_main, and the
 * self-check of a new image (60 s of uptime, issue #144) would pass at once. */
static uint32_t now_ms(void)
{
    return (uint32_t)((uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS);
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
 * between and have its "downloading" overwritten by this one's "failed"). */
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
    out->trial = gw_ota_trial_open(&s_trial);
    out->expected = out->trial ? s_trial.expected : GW_OTA_MODE_NONE;
    out->boot_count = gw_bootloop_count(&s_loop);
    portEXIT_CRITICAL(&s_mux);
    text_unlock(taken);
}

bool gw_fw_on_trial(void)
{
    portENTER_CRITICAL(&s_mux);
    bool open = gw_ota_trial_open(&s_trial);
    portEXIT_CRITICAL(&s_mux);
    return open;
}

uint32_t gw_fw_boot_count(void)
{
    return gw_bootloop_count(&s_loop);
}

/* ---------- issue #144 item 5: the record of the last update (fw_last) ---------- */

/* Written before each step that can end in a restart, and with each result. NVS refuses writes
 * after a factory reset (gw_storage_set_str): then the record stays as it was. */
static void last_write(gw_fwlast_result_t result, const char *version, gw_ota_mode_t expect,
                       const char *cause)
{
    char text[GW_FWLAST_MAX];
    portENTER_CRITICAL(&s_mux);
    s_last.result = result;
    s_last.expect = expect;
    snprintf(s_last.version, sizeof(s_last.version), "%s", version != NULL ? version : "");
    snprintf(s_last.cause, sizeof(s_last.cause), "%s", cause != NULL ? cause : "");
    gw_fwlast_format(&s_last, text, sizeof(text));
    portEXIT_CRITICAL(&s_mux);
    esp_err_t e = gw_storage_set_str(NVS_FW_LAST, text);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "record of the last update not written (%s): %s", esp_err_to_name(e), text);
    }
}

/* ---------- failures and refusals ---------- */

static void fail(const char *version, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* The transfer failed: the running image stays, nothing changes at boot. */
static void fail(const char *version, const char *fmt, ...)
{
    char why[sizeof(s_st.detail)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "firmware update failed: %s", why);
    last_write(GW_FWLAST_FAILED, version, GW_OTA_MODE_NONE, why);
    /* whoever sees the failed state may start the next update right away: the flag drops with
     * the phase, under the same lock (issue #101) */
    set_status_release(GW_FW_FAILED, s_st.progress, why, "failed: %s", why);
}

/* A refused request (nothing written). Issue #101: it does not hide a running transfer's state;
 * during a trial the state stays "testing <v>" (the HTTP answer and the log give the refusal). */
static void refused(const char *why)
{
    ESP_LOGW(TAG, "firmware update refused: %s", why);
    portENTER_CRITICAL(&s_mux);
    bool quiet = atomic_load(&s_busy) || gw_ota_trial_open(&s_trial);
    portEXIT_CRITICAL(&s_mux);
    if (!quiet) {
        set_status(GW_FW_FAILED, 0, why, "failed: %s", why);
    }
}

/* ---------- restarts ---------- */

/* A restart that gw_fw plans (not the operator's): the boot-loop counter stays. */
static void planned_restart(void)
{
    atomic_store(&s_planned, true);
    if (s_hooks.restart != NULL) {
        s_hooks.restart();   /* orderly: NDEATH first in normal mode */
    } else {
        gw_bus_quiesce(1000);   /* no-op without the bus */
        esp_restart();
    }
}

static void on_restart(void *arg)
{
    (void)arg;
    planned_restart();
}

/* An orderly restart in RESTART_DELAY_MS: the state NDATA (or the HTTP answer) goes out first. */
static void restart_soon(void)
{
    atomic_store(&s_planned, true);
    /* created when needed: no allocation in a normal boot. Issue #104: a new image on trial has
     * it from gw_fw_init(), so the restart after its confirmation (main task) allocates nothing */
    if (s_restart_timer == NULL) {
        const esp_timer_create_args_t rt = {.callback = on_restart, .name = "fw_restart"};
        if (esp_timer_create(&rt, &s_restart_timer) != ESP_OK) {
            ESP_LOGE(TAG, "no memory for the restart timer: restarting now");
            planned_restart();
            return;
        }
    }
    esp_timer_start_once(s_restart_timer, RESTART_DELAY_MS * 1000ULL);
}

/* ---------- issue #144 item 1: the trial of a new image ---------- */

static const char *other_version(void)
{
    return s_st.other[0] ? s_st.other : "the earlier firmware";
}

/* The image rejected itself: the cause goes to fw_last, then a planned restart. The image is
 * still pending, so the bootloader boots the earlier one (esp_ota_mark_app_invalid_rollback_
 * and_reboot() is not called: no verification of the earlier image in the app, whatever the
 * stack of the caller, issues #108, #109, #118). Runs in the main task, or in the esp_timer task
 * for the backstop (shallow: an NVS write and a request). */
static void reject_now(void)
{
    char cause[GW_OTA_CAUSE_MAX];
    portENTER_CRITICAL(&s_mux);
    snprintf(cause, sizeof(cause), "%s", s_trial.cause);
    gw_ota_mode_t expected = s_trial.expected;
    portEXIT_CRITICAL(&s_mux);
    if (s_backstop_timer != NULL) {
        esp_timer_stop(s_backstop_timer);
    }
    last_write(GW_FWLAST_REJECTED, s_st.running, expected, cause);
    char detail[sizeof(s_st.detail)];
    snprintf(detail, sizeof(detail), "firmware %s rejected (%s): restarting into %s",
             s_st.running, cause, other_version());
    ESP_LOGE(TAG, "%s", detail);
    set_status(GW_FW_RESTARTING, 0, detail, "restarting into %s", other_version());
    planned_restart();
}

/* The texts of a confirmation (or of its failure). */
static void confirm_report(esp_err_t e, bool nvs_restart)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t attempts = s_trial.attempts;
    uint32_t retry = s_trial.retry_at_ms;
    portEXIT_CRITICAL(&s_mux);
    char detail[sizeof(s_st.detail)];
    if (e != ESP_OK) {
        int32_t in = (int32_t)(retry - now_ms());
        snprintf(detail, sizeof(detail), "firmware %.31s passed the self-check, not marked valid "
                 "(%.32s, attempt %lu): retry in %ld s; a restart first rolls it back",
                 s_st.running, esp_err_to_name(e), (unsigned long)attempts,
                 (long)((in > 0 ? in : 0) + 999) / 1000);
        ESP_LOGE(TAG, "%s", detail);
        set_status(GW_FW_TESTING, 100, detail, "testing %s", s_st.running);
        return;
    }
    snprintf(detail, sizeof(detail), "firmware %s installed: the self-check passed in %s mode",
             s_st.running, gw_ota_mode_name(s_mode));
    ESP_LOGW(TAG, "%s; the bootloader keeps it", detail);
    set_status(GW_FW_INSTALLED, 100, detail, "installed %s", s_st.running);
    if (nvs_restart && gw_storage_nvs_migrates_next_boot()) {
        /* issue #94: the trial ran on the unencrypted NVS (a rollback had to find it as the
         * previous image left it); now no rollback can come, and the next boot moves it into
         * the encrypted NVS. Issue #104: not when that boot could not migrate either (no
         * LittleFS for the reset-safe copy): the restart would be wasted */
        ESP_LOGW(TAG, "firmware %s confirmed: restarting once to encrypt the NVS", s_st.running);
        restart_soon();
    } else if (nvs_restart && gw_storage_nvs_encryption_pending()) {
        ESP_LOGW(TAG, "firmware %s confirmed: the NVS stays unencrypted (no LittleFS for the "
                      "migration copy); no restart for it", s_st.running);
    }
}

/* Mark the running image valid. Issue #99: the image is CONFIRMED only when the write succeeded;
 * otherwise it stays on trial and the write is retried with a backoff, since any restart before
 * it succeeds would roll it back. On core 0 (QEMU's parked core must be core 1). @p nvs_restart:
 * restart once for the NVS encryption (issue #94) when it is pending; not when the caller
 * restarts anyway. */
static bool confirm(bool nvs_restart)
{
    xSemaphoreTake(s_ota_lock, portMAX_DELAY);
    park(true);
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    park(false);
    xSemaphoreGive(s_ota_lock);
    portENTER_CRITICAL(&s_mux);
    gw_ota_trial_confirm_result(&s_trial, e == ESP_OK, now_ms(), esp_random());
    portEXIT_CRITICAL(&s_mux);
    if (e == ESP_OK) {
        if (s_backstop_timer != NULL) {
            esp_timer_stop(s_backstop_timer);
        }
        last_write(GW_FWLAST_INSTALLED, s_st.running, GW_OTA_MODE_NONE, "");
        /* issue #146: the first change of the configuration keeps the one of the image that ran
         * before (the other slot) */
        gw_storage_cfgprev_arm(s_st.other, s_st.running);
        /* issue #144 item 8: the recovery network only from a confirmed image in normal mode */
        if (s_mode == GW_OTA_MODE_NORMAL && s_cfg != NULL) {
            gw_storage_save_last_good(s_cfg);
        }
    }
    confirm_report(e, nvs_restart);
    return e == ESP_OK;
}

typedef struct {
    bool nvs_restart;
    bool ok;
} confirm_call_t;

static void confirm_on_core0(void *arg)
{
    confirm_call_t *c = arg;
    c->ok = confirm(c->nvs_restart);
}

static void act(gw_ota_action_t a, bool nvs_restart)
{
    if (a == GW_OTA_CONFIRM) {
        confirm_call_t c = {.nvs_restart = nvs_restart};
        on_core0(confirm_on_core0, &c);
    } else if (a == GW_OTA_REJECT) {
        reject_now();
    }
}

static bool trial_open(void)
{
    portENTER_CRITICAL(&s_mux);
    bool open = gw_ota_trial_open(&s_trial);
    portEXIT_CRITICAL(&s_mux);
    return open;
}

void gw_fw_trial_poll(void)
{
    if (!trial_open()) {
        return;
    }
    gw_ota_selfcheck_t sc = {.net_started = false, .web_needed = false, .web_alive = false};
    if (s_hooks.self_check != NULL) {
        s_hooks.self_check(&sc);
    }
    portENTER_CRITICAL(&s_mux);
    gw_ota_action_t a = gw_ota_trial_check(&s_trial, now_ms(), &sc);
    portEXIT_CRITICAL(&s_mux);
    act(a, true);
}

void gw_fw_trial_wait(void)
{
    while (true) {
        portENTER_CRITICAL(&s_mux);
        bool waiting = s_trial.state == GW_OTA_TRIAL_TESTING ||
                       s_trial.state == GW_OTA_TRIAL_CONFIRMING;
        portEXIT_CRITICAL(&s_mux);
        if (!waiting) {
            return;
        }
        gw_fw_trial_poll();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* The backstop of a blocked main task: the self-check did not run to its end. */
static void on_backstop(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    gw_ota_action_t a = gw_ota_trial_backstop(&s_trial, "main task blocked");
    portEXIT_CRITICAL(&s_mux);
    if (a == GW_OTA_REJECT) {
        reject_now();
    }
}

static void on_loop_clear(void *arg)
{
    (void)arg;
    if (gw_bootloop_count(&s_loop) != 0) {
        ESP_LOGI(TAG, "boot-loop counter cleared: %d s in normal mode", CONFIG_GW_FW_BOOTLOOP_CLEAR_S);
    }
    gw_bootloop_set(&s_loop, 0);
}

static void on_safe_end(void *arg)
{
    (void)arg;
    gw_bootloop_set(&s_loop, GW_BOOTLOOP_SAFE - 1);
    ESP_LOGW(TAG, "safe mode ends after %d min: restarting to try normal mode again (one more "
                  "crash starts safe mode again)", CONFIG_GW_FW_SAFE_MODE_MIN);
    planned_restart();
}

static void loop_timer(esp_timer_cb_t cb, const char *name, uint64_t us)
{
    const esp_timer_create_args_t a = {.callback = cb, .name = name};
    if (s_loop_timer == NULL && esp_timer_create(&a, &s_loop_timer) != ESP_OK) {
        ESP_LOGE(TAG, "no memory for the %s timer", name);
        return;
    }
    esp_timer_start_once(s_loop_timer, us);
}

void gw_fw_mode(gw_ota_mode_t mode, const char *detail)
{
    s_mode = mode;
    portENTER_CRITICAL(&s_mux);
    gw_ota_action_t a = gw_ota_trial_mode(&s_trial, mode, detail);
    portEXIT_CRITICAL(&s_mux);
    if (mode == GW_OTA_MODE_NORMAL && !s_safe) {
        loop_timer(on_loop_clear, "bootloop", CONFIG_GW_FW_BOOTLOOP_CLEAR_S * 1000000ULL);
    }
    act(a, true);
}

void gw_fw_safe_mode(void)
{
    s_mode = GW_OTA_MODE_RECOVERY;
    loop_timer(on_safe_end, "safe_mode", CONFIG_GW_FW_SAFE_MODE_MIN * 60ULL * 1000000ULL);
}

void gw_fw_cannot_start(const char *cause)
{
    portENTER_CRITICAL(&s_mux);
    gw_ota_action_t a = gw_ota_trial_cannot_start(&s_trial, cause);
    portEXIT_CRITICAL(&s_mux);
    if (a != GW_OTA_REJECT) {
        return;
    }
    /* nothing runs that a restart must close: write the cause and restart at once */
    last_write(GW_FWLAST_REJECTED, s_st.running, s_trial.expected, s_trial.cause);
    ESP_LOGE(TAG, "firmware %s rejected (%s): restarting into %s", s_st.running, s_trial.cause,
             other_version());
    atomic_store(&s_planned, true);
    esp_restart();
}

/* Issue #99: one more synchronous try of a confirmation that keeps failing, before a planned
 * restart rolls the image back. */
static void restart_confirm_on_core0(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_mux);
    gw_ota_action_t a = gw_ota_trial_before_restart(&s_trial);
    portEXIT_CRITICAL(&s_mux);
    if (a == GW_OTA_CONFIRM && !confirm(false)) {   /* the caller restarts */
        ESP_LOGE(TAG, "firmware %s still not marked valid: the restart rolls it back",
                 s_st.running);
    }
}

void gw_fw_restart_requested(void)
{
    on_core0(restart_confirm_on_core0, NULL);
    if (!atomic_load(&s_planned) && gw_bootloop_count(&s_loop) != 0) {
        /* issue #144 item 4: an operator action clears the boot-loop counter */
        ESP_LOGI(TAG, "boot-loop counter cleared: restart asked by the operator");
        gw_bootloop_set(&s_loop, 0);
    }
}

/* Issue #144 item 1: during the trial a configuration save is refused (the restart that applies
 * it would roll the image back); a factory reset stays allowed. This removes the rule of #99 "a
 * saved configuration confirms the image". */
static esp_err_t on_commit(gw_storage_commit_t what, char *why, size_t cap)
{
    if (what == GW_STORAGE_COMMIT_FACTORY_RESET) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_mux);
    bool open = gw_ota_trial_open(&s_trial);
    portEXIT_CRITICAL(&s_mux);
    if (!open) {
        return ESP_OK;
    }
    gw_ota_trial_t t;
    portENTER_CRITICAL(&s_mux);
    t = s_trial;
    portEXIT_CRITICAL(&s_mux);
    gw_ota_trial_refusal(&t, s_st.running, now_ms(), why, cap);
    return ESP_ERR_INVALID_STATE;
}

/* ---------- issue #144 item 3: the pre-flight check ---------- */

/* The mode that an update from now must reach. */
static gw_ota_mode_t expected_now(bool force)
{
    return gw_ota_expected_mode(s_safe ? GW_OTA_MODE_RECOVERY : s_mode, force);
}

/* Issue #102 and #144: the capabilities block of an incoming image (GW_OTA_CAPS_SIZE bytes)
 * against this device. Before any write past the head: ESP_ERR_INVALID_ARG (NVS encryption,
 * #102: no force), ESP_ERR_NOT_SUPPORTED (the pre-flight check refused and no @p force). */
static esp_err_t check_image(const uint8_t *block, const char *version, bool force, char *why,
                             size_t cap)
{
    gw_ota_caps_info_t img;
    gw_ota_caps_parse(block, GW_OTA_CAPS_SIZE, &img);
    /* issue #146: the texts and the report from the heap (PSRAM), not from the stack of the
     * caller (the web server, or the Sparkplug task for the NCMD) */
    typedef struct {
        gw_schema_report_t doc;
        char reason[320];
        char rest[160];
    } pf_work_t;
    pf_work_t *w = gw_storage_malloc(sizeof(*w));
    char small[200];
    char *reason = w != NULL ? w->reason : small;
    size_t rcap = w != NULL ? sizeof(w->reason) : sizeof(small);
    if (!gw_ota_caps_accept(&img, gw_storage_nvs_encrypted_on_flash(), reason, rcap)) {
        ESP_LOGE(TAG, "update refused: the image %s; installing it would lose the NVS (secrets)",
                 reason);
        snprintf(why, cap, "image %s refused: %s", version, reason);
        heap_caps_free(w);
        return ESP_ERR_INVALID_ARG;
    }
    bool have_doc = w != NULL && gw_storage_config_schema(&w->doc);
    gw_ota_pf_t pf = gw_ota_preflight(&s_own, &img, have_doc ? &w->doc : NULL, reason, rcap);
    s_restore = false;
    if (pf == GW_OTA_PF_REFUSE && have_doc && w->doc.need > img.cfg_schema) {
        /* issue #146: the image cannot read the stored configuration; the configuration from
         * before the update to this firmware may be the one that it read */
        gw_cfgprev_t prev;
        uint32_t hash = 0;
        gw_cfgprev_action_t act = gw_storage_cfgprev_get(&prev) && gw_storage_config_hash(&hash)
                                      ? gw_cfgprev_decide(&prev, version, img.cfg_schema, hash)
                                      : GW_CFGPREV_NONE;
        if (act == GW_CFGPREV_RESTORE) {
            /* the restore fixes only the schema: the NVS and LittleFS checks still apply */
            gw_schema_init(&w->doc, NULL, 0);
            w->doc.valid = true;
            w->doc.need = prev.schema;
            if (gw_ota_preflight(&s_own, &img, &w->doc, w->rest, sizeof(w->rest)) ==
                GW_OTA_PF_OK) {
                s_restore = true;
                s_restore_hash = hash;
                pf = GW_OTA_PF_OK;
                ESP_LOGW(TAG, "pre-flight check: image %s reads the configuration from before the "
                              "update (unchanged since): it is restored before the restart",
                         version);
            }
        } else if (act == GW_CFGPREV_CHANGED) {
            size_t n = strlen(reason);
            int k = snprintf(reason + n, rcap - n, "; the configuration from before the update to "
                             "this firmware is not restored: it changed since (");
            if (k > 0 && n + (size_t)k < rcap) {
                n += (size_t)k;
                gw_storage_cfgprev_diff(reason + n, rcap - n);
                n = strlen(reason);
                snprintf(reason + n, rcap - n, ")");
            }
        }
    }
    esp_err_t ret = ESP_OK;
    if (pf != GW_OTA_PF_OK && !force) {
        /* issue #192: the web UI (index.html, fwref) reads this form: the version, the
         * reasons with "; " between them, and the last sentence */
        snprintf(why, cap, "pre-flight check: image %s %s: %s; force installs it into recovery "
                 "mode", version, pf == GW_OTA_PF_UNKNOWN ? "is unknown" : "refused", reason);
        ESP_LOGE(TAG, "update refused by the %s", why);
        ret = ESP_ERR_NOT_SUPPORTED;
    } else if (pf != GW_OTA_PF_OK) {
        ESP_LOGW(TAG, "pre-flight check: image %s %s (%s); forced: it must reach recovery mode",
                 version, pf == GW_OTA_PF_UNKNOWN ? "is unknown" : "refused", reason);
    }
    heap_caps_free(w);
    return ret;
}

/* Issue #146: before the boot partition of an earlier image, the configuration from before the
 * update (planned by check_image()). */
static esp_err_t prev_restore(char *why, size_t cap)
{
    return s_restore ? gw_storage_cfgprev_restore(s_restore_hash, why, cap) : ESP_OK;
}

/* The earlier image cannot boot after all: the configuration of this firmware again. */
static void prev_undo(void)
{
    if (s_restore) {
        s_restore = false;
        gw_storage_cfgprev_undo();
    }
}

static bool take(char *why, size_t cap)
{
    if (!s_enabled) {
        snprintf(why, cap, "firmware updates are disabled (ota.enabled)");
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    gw_ota_trial_t t = s_trial;
    portEXIT_CRITICAL(&s_mux);
    if (gw_ota_trial_open(&t)) {
        gw_ota_trial_refusal(&t, s_st.running, now_ms(), why, cap);
        return false;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        snprintf(why, cap, "an update is already running");
        return false;
    }
    s_restore = false;
    return true;
}

/* The new image is written, verified and set to boot: restart into it. */
static void finished(const char *version)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(s_st.other, sizeof(s_st.other), "%s", version);
    s_st.other_state = GW_FW_SLOT_NEW;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "firmware %s written and verified; restarting into it (on trial: the self-check "
                  "confirms it after %d s; it must reach %s mode)",
             version, CONFIG_GW_FW_SELFCHECK_S,
             gw_ota_mode_name(expected_now(s_force)));
    set_status(GW_FW_RESTARTING, 100, NULL, "restarting into %s", version);
    restart_soon();
}

static const char *project(void)
{
    return esp_app_get_description()->project_name;
}

/* Buffers of a transfer: PSRAM (gw_storage_malloc, #135); freed after it. */
static void *xfer_alloc(size_t size)
{
    return gw_storage_malloc(size);
}

/* ---------- download (its own task) ---------- */

static void download_release(void)
{
    gw_storage_free(s_ca_pem);
    s_ca_pem = NULL;
    heap_caps_free(s_url);
    s_url = NULL;
}

static void download_fail(const char *version, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* A failed download ends its task. The URL and the CA copy go back to the heap first: the
 * failed state lets the next update start at once, and it allocates its own. */
static void download_fail(const char *version, const char *fmt, ...)
{
    char why[sizeof(s_st.detail)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    download_release();
    fail(version, "%s", why);
    vTaskDelete(NULL);
}

/* Issues #102 and #144: the head of the image is in the update slot; the download ends (task
 * deleted) if this device must not install it. */
static void check_download_caps(esp_https_ota_handle_t h, const char *version)
{
    uint8_t caps[GW_OTA_CAPS_SIZE];
    char reason[sizeof(s_st.detail)];
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    esp_err_t r = slot != NULL ? esp_partition_read(slot, GW_OTA_CAPS_OFFSET, caps, sizeof(caps))
                               : ESP_ERR_NOT_FOUND;
    if (r != ESP_OK) {
        esp_https_ota_abort(h);
        download_fail(version, "image %s: its head could not be read back (%s)", version,
                      esp_err_to_name(r));
    }
    if (check_image(caps, version, s_force, reason, sizeof(reason)) != ESP_OK) {
        esp_https_ota_abort(h);
        download_fail(version, "%s", reason);
    }
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
        download_fail(NULL, "cannot download %s (%s)", s_url, esp_err_to_name(e));
    }
    esp_app_desc_t d;
    e = esp_https_ota_get_img_desc(h, &d);
    if (e != ESP_OK || strncmp(d.project_name, project(), sizeof(d.project_name)) != 0) {
        esp_https_ota_abort(h);   /* frees h: only the local copies are used below */
        h = NULL;
        if (e != ESP_OK) {
            download_fail(NULL, "not an application image (%s)", esp_err_to_name(e));
        }
        download_fail(NULL, "image of '%.32s', not of '%s'", d.project_name, project());
    }
    char version[sizeof(d.version) + 1];
    snprintf(version, sizeof(version), "%.32s", d.version);
    int size = esp_https_ota_get_image_size(h);
    uint32_t total = size > 0 ? (uint32_t)size : 0;
    ESP_LOGW(TAG, "downloading firmware %s (%lu bytes) from %s", version, (unsigned long)total,
             s_url);
    set_status(GW_FW_DOWNLOADING, 0, NULL, "downloading %s", version);
    gw_ota_progress_t p;
    gw_ota_progress_init(&p, total, now_ms());
    uint8_t pct;
    /* the first call runs esp_ota_begin(), which reads otadata through a mapping (rollback
     * state of the running image): the other core is parked for it too (QEMU) */
    park(true);
    e = esp_https_ota_perform(h);
    park(false);
    if (e == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        check_download_caps(h, version);   /* the first call wrote the head (1 KB) to the slot */
    }
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
        download_fail(version, "download of %s interrupted after %lu bytes (%s)", version, got,
                      esp_err_to_name(e));
    }
    /* issue #144 items 2 and 5: the expected mode is stored before the boot partition */
    last_write(GW_FWLAST_RESTARTING, version, expected_now(s_force), "");
    char rwhy[120];
    if (prev_restore(rwhy, sizeof(rwhy)) != ESP_OK) {   /* issue #146 */
        esp_https_ota_abort(h);
        h = NULL;
        download_fail(version, "image %s not installed: %s", version, rwhy);
    }
    park(true);
    e = esp_https_ota_finish(h);   /* verifies the image and sets it to boot; frees h always */
    h = NULL;
    park(false);
    if (e != ESP_OK) {
        prev_undo();
    }
    if (e == ESP_ERR_OTA_VALIDATE_FAILED) {
        download_fail(version, "image %s rejected (%s)", version, WHY_INVALID);
    } else if (e != ESP_OK) {
        download_fail(version, "image %s not installed (%s)", version, esp_err_to_name(e));
    }
    download_release();
    set_bytes(total ? total : p.done, total);
    finished(version);
    vTaskDelete(NULL);
}

esp_err_t gw_fw_start_url(const char *url, bool force, char *why, size_t cap)
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
    s_force = force;
    s_url = xfer_alloc(GW_OTA_URL_MAX);
    if (s_url == NULL) {
        snprintf(why, cap, "no memory for the update");
        ESP_LOGE(TAG, "firmware update refused: %s", why);
        set_status_release(GW_FW_FAILED, 0, why, "failed: no memory for the update");
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
            set_status_release(GW_FW_FAILED, 0, why, "failed: CA file unavailable");
            return ESP_ERR_INVALID_STATE;
        }
    }
    snprintf(s_url, GW_OTA_URL_MAX, "%s", url);
    set_bytes(0, 0);
    set_status(GW_FW_DOWNLOADING, 0, NULL, "downloading");
    if (xTaskCreatePinnedToCore(download_task, "fw_update", CONFIG_GW_FW_TASK_STACK, NULL,
                                CONFIG_GW_FW_TASK_PRIORITY, NULL, 0) != pdPASS) {
        download_release();
        fail(NULL, "no memory for the update task");
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

esp_err_t gw_fw_upload_begin(size_t size, bool force, char *why, size_t cap)
{
    if (!take(why, cap)) {
        refused(why);
        return ESP_ERR_INVALID_STATE;
    }
    s_force = force;
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
    ESP_LOGW(TAG, "receiving a firmware upload (%u bytes) into %s%s", (unsigned)size,
             s_up_part->label, force ? " (forced)" : "");
    set_status(GW_FW_DOWNLOADING, 0, NULL, "downloading");
    return ESP_OK;
}

static esp_err_t up_fail(esp_err_t ret, char *why, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static esp_err_t up_fail(esp_err_t ret, char *why, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
    esp_ota_abort(s_up);
    up_release();
    fail(s_up_version[0] ? s_up_version : NULL, "%s", why);
    return ret;
}

/* The image starts with this project's app description (checked before anything is written
 * past it, like esp_https_ota does), and its capabilities fit this device (#102, #144). */
static esp_err_t check_head(char *why, size_t cap)
{
    const esp_image_header_t *h = (const esp_image_header_t *)s_up_head;
    const esp_app_desc_t *d = (const esp_app_desc_t *)(s_up_head + sizeof(esp_image_header_t) +
                                                       sizeof(esp_image_segment_header_t));
    if (h->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return up_fail(ESP_FAIL, why, cap, "not an application image");
    }
    if (strncmp(d->project_name, project(), sizeof(d->project_name)) != 0) {
        return up_fail(ESP_FAIL, why, cap, "image of '%.32s', not of '%s'", d->project_name,
                       project());
    }
    snprintf(s_up_version, sizeof(s_up_version), "%.31s", d->version);
    char reason[sizeof(s_st.detail)];
    esp_err_t c = check_image(s_up_head + GW_OTA_CAPS_OFFSET, s_up_version, s_force, reason,
                              sizeof(reason));
    if (c != ESP_OK) {
        return up_fail(c == ESP_ERR_NOT_SUPPORTED ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL, why, cap,
                       "%s", reason);
    }
    set_status(GW_FW_DOWNLOADING, 0, NULL, "downloading %s", s_up_version);
    esp_err_t e = esp_ota_write(s_up, s_up_head, s_up_head_len);
    if (e != ESP_OK) {
        return up_fail(ESP_FAIL, why, cap, "flash write failed (%s)", esp_err_to_name(e));
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
        if (s_up_head_len == HEAD_LEN) {
            esp_err_t c = check_head(why, cap);
            if (c != ESP_OK) {
                return c;
            }
        }
    }
    if (len > 0) {
        esp_err_t e = esp_ota_write(s_up, p, len);
        if (e != ESP_OK) {
            return up_fail(ESP_FAIL, why, cap, "flash write failed after %u bytes (%s)",
                           (unsigned)s_up_done, esp_err_to_name(e));
        }
        s_up_done += len;
    }
    set_bytes((uint32_t)s_up_done, (uint32_t)s_up_total);
    uint8_t pct = s_up_total ? (uint8_t)((uint64_t)s_up_done * 100u / s_up_total) : 0;
    if (pct / 10 != s_up_pct / 10) {
        s_up_pct = pct;
        set_status(GW_FW_DOWNLOADING, pct, NULL, "downloading %s", s_up_version);
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
    xSemaphoreTake(s_ota_lock, portMAX_DELAY);
    park(true);
    in->err = esp_ota_end(s_up);   /* verifies the image */
    in->verified = in->err == ESP_OK;
    if (in->verified) {
        in->err = esp_ota_set_boot_partition(s_up_part);
    }
    park(false);
    xSemaphoreGive(s_ota_lock);
}

esp_err_t gw_fw_upload_end(bool complete, char *why, size_t cap)
{
    if (!complete || s_up_head_len < HEAD_LEN) {
        snprintf(why, cap, complete ? "image too short" : "upload interrupted after %u bytes",
                 (unsigned)s_up_done);
        esp_ota_abort(s_up);
        up_release();
        fail(s_up_version[0] ? s_up_version : NULL, "%s", why);
        return ESP_FAIL;
    }
    /* issue #144 items 2 and 5: the expected mode is stored before the boot partition */
    last_write(GW_FWLAST_RESTARTING, s_up_version, expected_now(s_force), "");
    char rwhy[120];
    if (prev_restore(rwhy, sizeof(rwhy)) != ESP_OK) {   /* issue #146 */
        esp_ota_abort(s_up);
        snprintf(why, cap, "image %s not installed: %s", s_up_version, rwhy);
        fail(s_up_version, "%s", why);
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
        prev_undo();
        fail(s_up_version, "%s", why);
        return e;
    }
    finished(s_up_version);
    return ESP_OK;
}

/* ---------- issue #144 item 6: boot the other slot ---------- */

static const esp_partition_t *s_other_part;
static char s_other_version[33];
static bool s_other_force;

/* esp_ota_set_boot_partition() verifies the whole image (seconds, with the other core parked in
 * QEMU): in its own task, so that the caller (the web server or the Sparkplug task) does not
 * wait for it. */
static void boot_other_task(void *arg)
{
    (void)arg;
    const char *version = s_other_version;
    last_write(GW_FWLAST_RESTARTING, version, expected_now(s_other_force), "");
    char rwhy[120];
    if (prev_restore(rwhy, sizeof(rwhy)) != ESP_OK) {   /* issue #146 */
        fail(version, "firmware %s in %s not booted: %s", version, s_other_part->label, rwhy);
        vTaskDelete(NULL);
    }
    xSemaphoreTake(s_ota_lock, portMAX_DELAY);
    park(true);
    esp_err_t e = esp_ota_set_boot_partition(s_other_part);   /* verifies the image */
    park(false);
    xSemaphoreGive(s_ota_lock);
    if (e != ESP_OK) {
        prev_undo();
        s_other_invalid = e == ESP_ERR_OTA_VALIDATE_FAILED;
        portENTER_CRITICAL(&s_mux);
        if (s_other_invalid) {
            s_st.other_state = GW_FW_SLOT_INVALID;
        }
        portEXIT_CRITICAL(&s_mux);
        fail(version, "firmware %s in %s cannot boot (%s)", version, s_other_part->label,
             e == ESP_ERR_OTA_VALIDATE_FAILED ? WHY_INVALID : esp_err_to_name(e));
        vTaskDelete(NULL);
    }
    s_force = s_other_force;
    portENTER_CRITICAL(&s_mux);
    s_st.other_state = GW_FW_SLOT_NEW;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "firmware %s in %s set to boot; restarting into it (on trial: it must reach %s "
                  "mode)", version, s_other_part->label,
             gw_ota_mode_name(expected_now(s_other_force)));
    set_status(GW_FW_RESTARTING, 100, NULL, "restarting into %s", version);
    restart_soon();
    vTaskDelete(NULL);
}

esp_err_t gw_fw_boot_other(bool force, char *why, size_t cap)
{
    if (!take(why, cap)) {
        refused(why);
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    portENTER_CRITICAL(&s_mux);
    gw_fw_slot_t st = s_st.other_state;
    snprintf(s_other_version, sizeof(s_other_version), "%s", s_st.other);
    portEXIT_CRITICAL(&s_mux);
    if (other == NULL || st == GW_FW_SLOT_EMPTY || st == GW_FW_SLOT_INVALID) {
        atomic_store(&s_busy, false);
        snprintf(why, cap, "the other slot is %s",
                 st == GW_FW_SLOT_INVALID ? "invalid (its verification failed)" : "empty");
        refused(why);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t caps[GW_OTA_CAPS_SIZE];
    esp_err_t e = esp_partition_read(other, GW_OTA_CAPS_OFFSET, caps, sizeof(caps));
    if (e == ESP_OK) {
        e = check_image(caps, s_other_version, force, why, cap);
    } else {
        snprintf(why, cap, "the head of %s could not be read (%s)", other->label,
                 esp_err_to_name(e));
    }
    if (e != ESP_OK) {
        atomic_store(&s_busy, false);
        refused(why);
        return e == ESP_ERR_NOT_SUPPORTED ? e : ESP_ERR_INVALID_STATE;
    }
    s_other_part = other;
    s_other_force = force;
    ESP_LOGW(TAG, "booting the other slot: firmware %s in %s, on trial%s", s_other_version,
             other->label, force ? " (forced)" : "");
    set_status(GW_FW_RESTARTING, 0, NULL, "restarting into %s", s_other_version);
    if (xTaskCreatePinnedToCore(boot_other_task, "fw_update", CONFIG_GW_FW_TASK_STACK, NULL,
                                CONFIG_GW_FW_TASK_PRIORITY, NULL, 0) != pdPASS) {
        snprintf(why, cap, "no memory for the update task");
        fail(s_other_version, "%s", why);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ---------- boot ---------- */

static gw_reset_kind_t reset_kind(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return GW_RESET_POWERON;
    case ESP_RST_BROWNOUT: return GW_RESET_BROWNOUT;
    case ESP_RST_PWR_GLITCH: return GW_RESET_PWR_GLITCH;
    case ESP_RST_SW: return GW_RESET_SOFTWARE;
    case ESP_RST_EXT: return GW_RESET_EXTERNAL;
    case ESP_RST_PANIC: return GW_RESET_PANIC;
    case ESP_RST_INT_WDT: return GW_RESET_INT_WDT;
    case ESP_RST_TASK_WDT: return GW_RESET_TASK_WDT;
    case ESP_RST_WDT: return GW_RESET_WDT;
    case ESP_RST_CPU_LOCKUP: return GW_RESET_CPU_LOCKUP;
    default: return GW_RESET_OTHER;
    }
}

static bool running_pending(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return run != NULL && esp_ota_get_state_partition(run, &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool gw_fw_bootloop_start(void)
{
    bool pending = running_pending();
    esp_reset_reason_t why = esp_reset_reason();
    uint32_t before = gw_bootloop_count(&s_loop);
    s_loop_count = gw_bootloop_boot(&s_loop, reset_kind(why), pending);
    s_safe = gw_bootloop_safe(s_loop_count, pending);
    if (s_safe) {
        ESP_LOGE(TAG, "safe mode: %lu crash resets in a row (boot-loop counter); the configuration "
                      "is not loaded: network, web UI, shell and firmware upload only",
                 (unsigned long)s_loop_count);
    } else if (s_loop_count != 0 || before != 0) {
        ESP_LOGW(TAG, "boot-loop counter %lu (was %lu)%s", (unsigned long)s_loop_count,
                 (unsigned long)before, pending ? "; ignored by a new image on trial" : "");
    }
    return s_safe;
}

static const char *reset_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lock-up";
    default: return "other reset";
    }
}

static bool s_early;

void gw_fw_early_init(void)
{
    if (s_early) {
        return;
    }
    s_early = true;
    park_init();
    s_fmt_lock = xSemaphoreCreateMutexStatic(&s_fmt_lock_buf);
    s_ota_lock = xSemaphoreCreateMutexStatic(&s_ota_lock_buf);
    gw_ota_caps_parse((const uint8_t *)&gw_app_caps, sizeof(gw_app_caps), &s_own);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    bool pending = running_pending();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t od;
    park(true);
    bool have_other = other != NULL && esp_ota_get_partition_description(other, &od) == ESP_OK;
    park(false);
    esp_ota_img_states_t ost = ESP_OTA_IMG_UNDEFINED;
    bool ost_known = have_other && esp_ota_get_state_partition(other, &ost) == ESP_OK;
    bool other_rejected = ost_known && (ost == ESP_OTA_IMG_ABORTED || ost == ESP_OTA_IMG_INVALID);

    /* issue #144 item 5: the record of the last update, with the reset reason and otadata */
    char text[GW_FWLAST_MAX];
    gw_fwlast_t last;
    if (!gw_storage_get_str(NVS_FW_LAST, text, sizeof(text)) || !gw_fwlast_parse(text, &last)) {
        memset(&last, 0, sizeof(last));
    }
    const gw_fwlast_boot_t b = {.pending = pending, .running = app->version,
                                .other_rejected = other_rejected,
                                .reset = reset_text(esp_reset_reason())};
    gw_ota_mode_t expected = GW_OTA_MODE_NONE;
    bool write = gw_fwlast_on_boot(&last, &b, &expected);
    char state[sizeof(s_st.text)];
    gw_fwlast_state(&last, app->version, state, sizeof(state));

    /* issue #101: written under the locks the readers take (the shell may already ask) */
    bool taken = text_lock();
    portENTER_CRITICAL(&s_mux);
    s_last = last;
    snprintf(s_st.running, sizeof(s_st.running), "%s", app->version);
    snprintf(s_st.slot, sizeof(s_st.slot), "%s", run != NULL ? run->label : "?");
    snprintf(s_st.other, sizeof(s_st.other), "%.31s", have_other ? od.version : "");
    snprintf(s_st.other_slot, sizeof(s_st.other_slot), "%s", other != NULL ? other->label : "");
    s_st.other_state = !have_other                                            ? GW_FW_SLOT_EMPTY
                       : other_rejected                                       ? GW_FW_SLOT_REJECTED
                       : ost_known && (ost == ESP_OTA_IMG_NEW ||
                                       ost == ESP_OTA_IMG_PENDING_VERIFY)     ? GW_FW_SLOT_NEW
                                                                              : GW_FW_SLOT_PREVIOUS;
    s_st.other_why[0] = '\0';
    if (other_rejected && strcmp(last.version, s_st.other) == 0 &&
        (last.result == GW_FWLAST_ROLLED_BACK || last.result == GW_FWLAST_REJECTED)) {
        /* item 6: esp_ota_get_last_invalid_partition() forgets it when otadata changes */
        snprintf(s_st.other_why, sizeof(s_st.other_why), "%s", last.cause);
    }
    s_st.safe_mode = s_safe;
    gw_ota_trial_init(&s_trial, pending, expected, CONFIG_GW_FW_SELFCHECK_S * 1000u,
                      CONFIG_GW_FW_BACKSTOP_S * 1000u);
    portEXIT_CRITICAL(&s_mux);
    text_unlock(taken);
    if (write) {
        last_write(last.result, last.version, last.expect, last.cause);
    }

    if (pending) {
        ESP_LOGW(TAG, "firmware %s (%s) on trial: it must reach %s mode; the self-check confirms "
                      "it after %d s, otherwise back to %s at %d s",
                 s_st.running, s_st.slot, gw_ota_mode_name(s_trial.expected),
                 CONFIG_GW_FW_SELFCHECK_S, other_version(), CONFIG_GW_FW_BACKSTOP_S);
        set_status(GW_FW_TESTING, 100, NULL, "testing %s", s_st.running);
        return;
    }
    gw_fw_phase_t phase = last.result == GW_FWLAST_ROLLED_BACK ? GW_FW_ROLLED_BACK
                          : last.result == GW_FWLAST_FAILED    ? GW_FW_FAILED
                          : strncmp(state, "installed ", 10) == 0 ? GW_FW_INSTALLED
                                                                  : GW_FW_IDLE;
    if (phase == GW_FW_ROLLED_BACK) {
        char detail[sizeof(s_st.detail)];
        snprintf(detail, sizeof(detail), "firmware %s was rolled back (%s); running %s again",
                 last.version, last.cause, s_st.running);
        ESP_LOGW(TAG, "%s", detail);
        set_status(phase, 0, detail, "%s", state);
    } else {
        set_status(phase, 0, NULL, "%s", state);
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
    portEXIT_CRITICAL(&s_mux);
    if (pending) {
        /* only a new image on trial needs these (#80: nothing allocated otherwise). Issue #104:
         * the restart after the confirmation (NVS encryption) must not allocate */
        const esp_timer_create_args_t rt = {.callback = on_restart, .name = "fw_restart"};
        ESP_ERROR_CHECK(esp_timer_create(&rt, &s_restart_timer));
        /* the backstop of a blocked main task (it is not watched by the task watchdog) */
        const esp_timer_create_args_t bt = {.callback = on_backstop, .name = "fw_backstop"};
        ESP_ERROR_CHECK(esp_timer_create(&bt, &s_backstop_timer));
        uint32_t now = now_ms(), at = CONFIG_GW_FW_BACKSTOP_S * 1000u + BACKSTOP_GRACE_MS;
        esp_timer_start_once(s_backstop_timer, (uint64_t)(at > now ? at - now : 1u) * 1000ULL);
    }
    gw_storage_set_commit_hook(on_commit);   /* issue #144 item 1 */

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
