#include "gw_storage.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw_auth.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";

#define CFG_MOUNT   "/cfg"
#define CFG_PATH    CFG_MOUNT "/config.json"
#define CFG_TMP     CFG_MOUNT "/config.json.tmp"
#define CFG_MAX     CONFIG_GW_CONFIG_MAX_BYTES
#define NVS_NS      "spb"
#define NVS_BDSEQ   "bdseq"
#define NVS_SECRETS "secrets"     /* CFG-7: never in the JSON document */
#define NVS_GW      "gw"
#define NVS_LKG     "lkg_net"     /* network of the last valid configuration (recovery) */
#define NVS_FACTORY "factory"     /* set by a factory reset, cleared by the first save (CFG-5) */

extern const char default_config_json_start[] asm("_binary_default_config_json_start");
extern const char default_config_json_end[] asm("_binary_default_config_json_end");

static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static uint8_t s_bdseq_ram;  /* fallback if NVS is unusable */
static bool s_mounted;
static SemaphoreHandle_t s_save_lock;   /* one writer at a time: web, shell */
static StaticSemaphore_t s_save_lock_buf;

/* Configuration buffers (the file and the parser's document tree, at boot and for every
 * save or export): PSRAM when the board has it, internal RAM otherwise (QEMU). */
static void *boot_alloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

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
    s_save_lock = xSemaphoreCreateMutexStatic(&s_save_lock_buf);
    gw_config_set_allocator(boot_alloc, heap_caps_free);
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

static bool reject(gw_cfg_err_t *err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static bool reject(gw_cfg_err_t *err, const char *fmt, ...)
{
    memset(err, 0, sizeof(*err));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->msg, sizeof(err->msg), fmt, ap);
    va_end(ap);
    return false;
}

void gw_storage_free(void *ptr)
{
    heap_caps_free(ptr);
}

/* ---------- secrets (CFG-7) ---------- */

static const struct {
    const char *key;
    size_t offset;
    size_t size;
} k_secret_keys[] = {
    {"mqtt_pw", offsetof(gw_secrets_t, mqtt_password), GW_CRED_SIZE},
    {"wifi_pw", offsetof(gw_secrets_t, wifi_password), GW_PSK_SIZE},
    {"web_hash", offsetof(gw_secrets_t, web_password_hash), GW_PWHASH_SIZE},
};

void gw_storage_get_secrets(gw_secrets_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(NVS_SECRETS, NVS_READONLY, &h) != ESP_OK) {
        return;   /* nothing stored yet */
    }
    for (size_t i = 0; i < sizeof(k_secret_keys) / sizeof(k_secret_keys[0]); i++) {
        char *dst = (char *)out + k_secret_keys[i].offset;
        size_t len = k_secret_keys[i].size;
        if (nvs_get_str(h, k_secret_keys[i].key, dst, &len) != ESP_OK) {
            dst[0] = '\0';
        }
    }
    nvs_close(h);
}

void gw_storage_commissioning_password(char out[GW_COMMISSION_PW_LEN + 1])
{
    /* no 0/O, 1/l/i: it is read from a console and typed into a browser */
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    static char s_pw[GW_COMMISSION_PW_LEN + 1];
    if (s_pw[0] == '\0') {
        nvs_handle_t h;
        size_t len = sizeof(s_pw);
        bool open = s_nvs_ok && nvs_open(NVS_SECRETS, NVS_READWRITE, &h) == ESP_OK;
        if (!open || nvs_get_str(h, "commission", s_pw, &len) != ESP_OK ||
            strlen(s_pw) != GW_COMMISSION_PW_LEN) {
            for (int i = 0; i < GW_COMMISSION_PW_LEN; i++) {
                s_pw[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
            }
            s_pw[GW_COMMISSION_PW_LEN] = '\0';
            if (open && (nvs_set_str(h, "commission", s_pw) != ESP_OK || nvs_commit(h) != ESP_OK)) {
                ESP_LOGE(TAG, "commissioning password not persisted: it changes at every boot");
            }
        }
        if (open) {
            nvs_close(h);
        }
    }
    memcpy(out, s_pw, sizeof(s_pw));
}

static esp_err_t put_secrets(const gw_secrets_t *in)
{
    if (!s_nvs_ok) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_SECRETS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; err == ESP_OK && i < sizeof(k_secret_keys) / sizeof(k_secret_keys[0]);
         i++) {
        const char *v = (const char *)in + k_secret_keys[i].offset;
        if (v[0] == '\0') {
            err = nvs_erase_key(h, k_secret_keys[i].key);
            err = err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
        } else {
            err = nvs_set_str(h, k_secret_keys[i].key, v);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---------- network of the last valid configuration (CFG-3 recovery) ---------- */

typedef struct {
    uint32_t size;    /* sizeof(lkg_t): a blob from a firmware with another layout is ignored */
    __typeof__(((gw_config_t *)0)->network) network;
    uint16_t web_port;
} lkg_t;

void gw_storage_save_last_good(const gw_config_t *cfg)
{
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(NVS_GW, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    lkg_t now = {.size = sizeof(lkg_t), .web_port = cfg->web.port}, old;
    now.network = cfg->network;
    size_t len = sizeof(old);
    /* written only when it changed: NVS wear */
    if (nvs_get_blob(h, NVS_LKG, &old, &len) != ESP_OK || len != sizeof(old) ||
        memcmp(&old, &now, sizeof(now)) != 0) {
        if (nvs_set_blob(h, NVS_LKG, &now, sizeof(now)) != ESP_OK || nvs_commit(h) != ESP_OK) {
            ESP_LOGW(TAG, "network of the valid configuration not kept for recovery");
        }
    }
    nvs_close(h);
    memset(&now, 0, sizeof(now));
    memset(&old, 0, sizeof(old));
}

void gw_storage_recovery_config(gw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    lkg_t lkg;
    size_t len = sizeof(lkg);
    nvs_handle_t h;
    bool found = false;
    if (s_nvs_ok && nvs_open(NVS_GW, NVS_READONLY, &h) == ESP_OK) {
        found = nvs_get_blob(h, NVS_LKG, &lkg, &len) == ESP_OK && len == sizeof(lkg) &&
                lkg.size == sizeof(lkg);
        nvs_close(h);
    }
    if (found) {
        cfg->network = lkg.network;
        cfg->web.port = lkg.web_port;
        memset(&lkg, 0, sizeof(lkg));
    } else {
        cfg->network.dhcp = true;
        cfg->network.wifi.failback_s = 30;
        cfg->network.wifi.ipv4.dhcp = true;
        cfg->web.port = 80;
    }
    strcpy(cfg->ntp.server, "pool.ntp.org");
    cfg->web.enabled = true;
    strcpy(cfg->web.username, "admin");
    ESP_LOGW(TAG, "recovery network: %s", found ? "the last valid configuration's"
                                               : "defaults (DHCP on Ethernet)");
}

static bool mount(bool format_if_needed, esp_err_t *why);

/* ---------- factory reset (CFG-5) ---------- */

static void set_factory_marker(bool on)
{
    nvs_handle_t h;
    if (s_nvs_ok && nvs_open(NVS_GW, NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t e = on ? nvs_set_u8(h, NVS_FACTORY, 1) : nvs_erase_key(h, NVS_FACTORY);
        if (e == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

esp_err_t gw_storage_factory_reset(void)
{
    xSemaphoreTake(s_save_lock, portMAX_DELAY);   /* no save in the middle; never given back */
    ESP_LOGW(TAG, "factory reset: formatting LittleFS 'storage' and erasing NVS");
    if (s_mounted) {
        esp_vfs_littlefs_unregister("storage");
        s_mounted = false;
    }
    esp_err_t fs = esp_littlefs_format("storage");
    if (s_nvs_ok) {
        nvs_close(s_nvs);
    }
    nvs_flash_deinit();
    esp_err_t nv = nvs_flash_erase();
    if (nv == ESP_OK) {
        nv = nvs_flash_init();
    }
    s_nvs_ok = false;
    if (nv == ESP_OK) {
        s_nvs_ok = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs) == ESP_OK;
        set_factory_marker(true);   /* next boot: commissioning, not the embedded example */
    }
    ESP_LOGW(TAG, "factory reset: storage %s, NVS %s", esp_err_to_name(fs), esp_err_to_name(nv));
    return fs != ESP_OK ? fs : nv;
}

bool gw_storage_commissioning_pending(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (s_nvs_ok && nvs_open(NVS_GW, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_FACTORY, &v);
        nvs_close(h);
    }
    if (v == 0) {
        return false;
    }
    struct stat st;
    if (mount(false, NULL) && stat(CFG_PATH, &st) == 0) {
        set_factory_marker(false);   /* a configuration was flashed since the reset */
        return false;
    }
    return true;
}

/* ---------- configuration file ---------- */

static bool mount(bool format_if_needed, esp_err_t *why)
{
    if (s_mounted) {
        return true;
    }
    const esp_vfs_littlefs_conf_t conf = {
        .base_path = CFG_MOUNT,
        .partition_label = "storage",
        .format_if_mount_failed = format_if_needed,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (why != NULL) {
        *why = err;
    }
    s_mounted = err == ESP_OK;
    return s_mounted;
}

/* Whole file in a boot_alloc buffer (NUL terminated). NULL with the reason in @p err. */
static char *read_file(const struct stat *st, size_t *len, gw_cfg_err_t *err)
{
    memset(err, 0, sizeof(*err));
    if (st->st_size <= 0) {
        snprintf(err->msg, sizeof(err->msg), CFG_PATH ": archivo vacío");
        return NULL;
    }
    if (st->st_size > CFG_MAX) {
        snprintf(err->msg, sizeof(err->msg),
                 CFG_PATH ": %ld bytes; el máximo es %d (CONFIG_GW_CONFIG_MAX_BYTES)",
                 (long)st->st_size, CFG_MAX);
        return NULL;
    }
    size_t size = (size_t)st->st_size;
    char *buf = boot_alloc(size + 1);
    if (buf == NULL) {
        snprintf(err->msg, sizeof(err->msg), CFG_PATH ": sin memoria para leer %zu bytes", size);
        return NULL;
    }
    FILE *f = fopen(CFG_PATH, "rb");
    size_t got = f != NULL ? fread(buf, 1, size, f) : 0;
    int read_errno = errno;
    if (f != NULL) {
        fclose(f);
    }
    if (got != size) {
        snprintf(err->msg, sizeof(err->msg), CFG_PATH ": error de lectura (%zu de %zu bytes, errno %d)",
                 got, size, read_errno);
        heap_caps_free(buf);
        return NULL;
    }
    buf[size] = '\0';
    *len = size;
    return buf;
}

/* Temporary file + fsync + rename: a power cut leaves the old or the new document, never a
 * mix (LittleFS renames atomically, replacing the destination). */
static esp_err_t write_atomic(const char *text, size_t len)
{
    esp_err_t why = ESP_OK;
    if (!s_mounted && !mount(false, &why)) {
        /* nothing usable in the partition: saving is the way out of that state */
        ESP_LOGW(TAG, "LittleFS 'storage' not mountable (%s): formatting it to save",
                 esp_err_to_name(why));
        if (!mount(true, &why)) {
            ESP_LOGE(TAG, "LittleFS 'storage' unusable: %s", esp_err_to_name(why));
            return why;
        }
    }
    FILE *f = fopen(CFG_TMP, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, CFG_TMP ": errno %d", errno);
        return ESP_FAIL;
    }
    bool ok = fwrite(text, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(CFG_TMP, CFG_PATH) != 0) {
        ESP_LOGE(TAG, "writing " CFG_PATH " failed (errno %d)", errno);
        unlink(CFG_TMP);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* The current document: the file, or a copy of the embedded default only when the file does
 * not exist (CFG-2). NULL with the reason in @p err. Release with heap_caps_free. */
static char *current_doc(size_t *len, bool *from_file, gw_cfg_err_t *err)
{
    struct stat st;
    if (stat(CFG_PATH, &st) == 0) {
        *from_file = true;
        return read_file(&st, len, err);
    }
    if (errno != ENOENT) {
        reject(err, CFG_PATH ": no se puede consultar (errno %d)", errno);
        return NULL;
    }
    *from_file = false;
    /* EMBED_TXTFILES appends a NUL; strnlen excludes it. */
    size_t n = strnlen(default_config_json_start,
                       (size_t)(default_config_json_end - default_config_json_start));
    char *copy = boot_alloc(n + 1);
    if (copy == NULL) {
        reject(err, "sin memoria para la configuración embebida");
        return NULL;
    }
    memcpy(copy, default_config_json_start, n + 1);
    *len = n;
    return copy;
}

/* CFG-7: the web password is only ever stored as a salted PBKDF2 hash. */
static bool hash_web_password(gw_config_t *cfg, char *out, size_t cap)
{
    uint8_t salt[GW_AUTH_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    bool ok = gw_auth_hash_password(cfg->web.password, salt, GW_AUTH_ITERATIONS, out, cap);
    memset(cfg->web.password, 0, sizeof(cfg->web.password));
    if (ok) {
        strcpy(cfg->web.password_hash, out);
    }
    return ok;
}

bool gw_storage_load_config(gw_config_t *cfg, gw_cfg_err_t *err, const char **source)
{
    *source = CFG_PATH " (LittleFS)";
    esp_err_t why;
    if (!mount(false, &why)) {
        return reject(err, "partición LittleFS 'storage' no montable (%s)", esp_err_to_name(why));
    }
    size_t len;
    bool from_file;
    char *text = current_doc(&len, &from_file, err);
    if (text == NULL) {
        return false;
    }
    if (!from_file) {
        *source = "embedded default (no " CFG_PATH ")";
    }
    gw_secrets_t stored;
    gw_storage_get_secrets(&stored);
    gw_cfgdoc_t doc;
    bool ok = gw_cfgdoc_apply(text, len, GW_CFGDOC_FILE, &stored, cfg, &doc, err);
    heap_caps_free(text);
    if (!ok) {
        return false;
    }
    if (doc.new_web_password) {
        hash_web_password(cfg, doc.secrets.web_password_hash, sizeof(doc.secrets.web_password_hash));
    }
    if (from_file && doc.had_clear_secrets) {
        /* Migration (CFG-7): a file written by hand or by an older firmware carries secrets
         * in clear. They are used, moved to NVS and the file is rewritten without them. */
        esp_err_t e = put_secrets(&doc.secrets);
        if (e == ESP_OK) {
            e = write_atomic(doc.doc, doc.doc_len);
        }
        if (e == ESP_OK) {
            ESP_LOGW(TAG, "secrets moved from " CFG_PATH " to NVS; the file no longer holds them");
        } else {
            ESP_LOGE(TAG, "secrets in clear in " CFG_PATH " could not be moved to NVS (%s)",
                     esp_err_to_name(e));
        }
    }
    memset(&doc.secrets, 0, sizeof(doc.secrets));
    gw_cfgdoc_free(&doc);
    return true;
}

esp_err_t gw_storage_save_config(const char *json, size_t len, bool dry_run, gw_cfg_err_t *err)
{
    memset(err, 0, sizeof(*err));
    if (len > CFG_MAX) {
        reject(err, "%zu bytes; el máximo es %d (CONFIG_GW_CONFIG_MAX_BYTES)", len, CFG_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    gw_config_t *cfg = boot_alloc(sizeof(*cfg));
    if (cfg == NULL) {
        reject(err, "sin memoria para validar la configuración");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    gw_secrets_t stored;
    gw_storage_get_secrets(&stored);
    gw_cfgdoc_t doc;
    esp_err_t ret = ESP_OK;
    if (!gw_cfgdoc_apply(json, len, GW_CFGDOC_UPDATE, &stored, cfg, &doc, err)) {
        ret = strstr(err->msg, "memoria insuficiente") ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    } else if (!dry_run) {
        if (doc.new_web_password &&
            !hash_web_password(cfg, doc.secrets.web_password_hash,
                               sizeof(doc.secrets.web_password_hash))) {
            ret = ESP_FAIL;
        }
        /* secrets first: if the file cannot be written, the previous ones are put back */
        if (ret == ESP_OK && doc.secrets_changed && (ret = put_secrets(&doc.secrets)) != ESP_OK) {
            reject(err, "no se pudieron guardar los secretos en NVS (%s)", esp_err_to_name(ret));
        }
        if (ret == ESP_OK && (ret = write_atomic(doc.doc, doc.doc_len)) != ESP_OK) {
            reject(err, "no se pudo escribir " CFG_PATH " (%s)", esp_err_to_name(ret));
            if (doc.secrets_changed) {
                put_secrets(&stored);
            }
        }
        if (ret == ESP_OK) {
            set_factory_marker(false);
            ESP_LOGI(TAG, "configuration saved to " CFG_PATH " (%zu bytes)%s", doc.doc_len,
                     doc.secrets_changed ? "; secrets updated in NVS" : "");
        }
    }
    gw_cfgdoc_free(&doc);
    xSemaphoreGive(s_save_lock);
    memset(cfg, 0, sizeof(*cfg));   /* it held the secrets in clear */
    heap_caps_free(cfg);
    return ret;
}

char *gw_storage_export_config(size_t *len, gw_cfg_err_t *err)
{
    memset(err, 0, sizeof(*err));
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    char *out = NULL;
    esp_err_t why;
    if (!mount(false, &why)) {
        reject(err, "partición LittleFS 'storage' no montable (%s)", esp_err_to_name(why));
    } else {
        size_t n;
        bool from_file;
        char *text = current_doc(&n, &from_file, err);
        if (text != NULL) {
            gw_secrets_t stored;
            gw_storage_get_secrets(&stored);
            out = gw_cfgdoc_export(text, n, &stored, len, err);
            memset(&stored, 0, sizeof(stored));
            heap_caps_free(text);
        }
    }
    xSemaphoreGive(s_save_lock);
    return out;
}
