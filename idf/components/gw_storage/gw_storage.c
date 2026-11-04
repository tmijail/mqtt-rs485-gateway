#include "gw_storage.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_littlefs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_auth.h"
#include "gw_nvsraw.h"
#include "mbedtls/platform.h"
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
static volatile bool s_reset_done;    /* factory reset: no more state writes until the restart */
static SemaphoreHandle_t s_save_lock;   /* one writer at a time: web, shell */
static StaticSemaphore_t s_save_lock_buf;

/* Configuration buffers (the file and the parser's document tree, at boot and for every
 * save or export): PSRAM when the board has it, internal RAM otherwise (QEMU). */
static void *boot_alloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
/* MQTT-4: a TLS session takes some 40 KB of heap. In PSRAM when the board has it, internal
 * RAM otherwise (QEMU, PSRAM not detected): TLS must keep working without PSRAM. */
static void *tls_calloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
#endif

#if CONFIG_NVS_ENCRYPTION
/* ---------- issue #67: unencrypted NVS -> encrypted NVS on the first encrypted boot ----------
 * Initialising the encrypted NVS over an unencrypted one loses every entry (bdSeq, secrets,
 * commissioning password, web certificate, I/O state). Before that, the raw pages tell which
 * kind is there (gw_nvsraw, without initialising anything); an unencrypted one is read in
 * full (every namespace, every key), erased and written back encrypted. */
typedef struct {
    char ns[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    uint8_t type;            /* nvs_type_t */
    uint16_t len;            /* bytes that follow (integers: 8) */
} mig_rec_t;

static gw_nvsraw_kind_t nvs_raw_kind(const esp_partition_t *p)
{
    gw_nvsraw_scan_t sc;
    gw_nvsraw_scan_init(&sc);
    uint8_t *page = heap_caps_malloc(GW_NVSRAW_PAGE, MALLOC_CAP_8BIT);
    if (page == NULL) {
        return GW_NVSRAW_ENCRYPTED;   /* cannot tell: leave it to the normal initialisation */
    }
    for (uint32_t off = 0; off + GW_NVSRAW_PAGE <= p->size; off += GW_NVSRAW_PAGE) {
        if (esp_partition_read_raw(p, off, page, GW_NVSRAW_PAGE) == ESP_OK) {
            gw_nvsraw_scan_page(&sc, page);
        }
    }
    heap_caps_free(page);
    ESP_LOGD(TAG, "NVS raw scan: %lu pages, %lu written entries, %lu plain items, %lu other",
             (unsigned long)sc.pages, (unsigned long)sc.written, (unsigned long)sc.plain_ok,
             (unsigned long)sc.bad);
    return gw_nvsraw_verdict(&sc);
}

/* Every entry of the open, unencrypted NVS into @p buf (records back to back). */
static size_t nvs_read_all(uint8_t *buf, size_t cap, unsigned *count, unsigned *skipped)
{
    size_t used = 0;
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    while (e == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        nvs_handle_t h;
        mig_rec_t r = {.type = (uint8_t)info.type};
        snprintf(r.ns, sizeof(r.ns), "%s", info.namespace_name);
        snprintf(r.key, sizeof(r.key), "%s", info.key);
        bool opened = nvs_open_from_partition(NVS_DEFAULT_PART_NAME, r.ns, NVS_READONLY, &h) ==
                      ESP_OK;
        bool ok = opened;
        uint8_t *data = buf + used + sizeof(r);
        size_t room = cap > used + sizeof(r) ? cap - used - sizeof(r) : 0;
        size_t len = 0;
        if (ok && info.type == NVS_TYPE_STR) {
            len = room;
            ok = nvs_get_str(h, r.key, (char *)data, &len) == ESP_OK;
        } else if (ok && info.type == NVS_TYPE_BLOB) {
            len = room;
            ok = nvs_get_blob(h, r.key, data, &len) == ESP_OK;
        } else if (ok && room >= 8) {
            uint64_t v = 0;
            len = 8;
            switch (info.type) {
            case NVS_TYPE_U8: ok = nvs_get_u8(h, r.key, (uint8_t *)&v) == ESP_OK; break;
            case NVS_TYPE_I8: ok = nvs_get_i8(h, r.key, (int8_t *)&v) == ESP_OK; break;
            case NVS_TYPE_U16: ok = nvs_get_u16(h, r.key, (uint16_t *)&v) == ESP_OK; break;
            case NVS_TYPE_I16: ok = nvs_get_i16(h, r.key, (int16_t *)&v) == ESP_OK; break;
            case NVS_TYPE_U32: ok = nvs_get_u32(h, r.key, (uint32_t *)&v) == ESP_OK; break;
            case NVS_TYPE_I32: ok = nvs_get_i32(h, r.key, (int32_t *)&v) == ESP_OK; break;
            case NVS_TYPE_U64: ok = nvs_get_u64(h, r.key, &v) == ESP_OK; break;
            case NVS_TYPE_I64: ok = nvs_get_i64(h, r.key, (int64_t *)&v) == ESP_OK; break;
            default: ok = false; break;
            }
            memcpy(data, &v, 8);
        } else {
            ok = false;
        }
        if (opened) {
            nvs_close(h);
        }
        if (ok && len <= UINT16_MAX) {
            r.len = (uint16_t)len;
            memcpy(buf + used, &r, sizeof(r));
            used += sizeof(r) + len;
            (*count)++;
        } else {
            (*skipped)++;
            ESP_LOGW(TAG, "NVS migration: %s/%s (type 0x%02x) not copied", r.ns, r.key, r.type);
        }
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return used;
}

static unsigned nvs_write_all(const uint8_t *buf, size_t used)
{
    unsigned failed = 0;
    for (size_t off = 0; off < used;) {
        mig_rec_t r;
        memcpy(&r, buf + off, sizeof(r));
        const uint8_t *d = buf + off + sizeof(r);
        off += sizeof(r) + r.len;
        nvs_handle_t h;
        esp_err_t e = nvs_open_from_partition(NVS_DEFAULT_PART_NAME, r.ns, NVS_READWRITE, &h);
        if (e == ESP_OK) {
            uint64_t v;
            memcpy(&v, d, sizeof(v) <= r.len ? sizeof(v) : r.len);
            switch (r.type) {
            case NVS_TYPE_STR: e = nvs_set_str(h, r.key, (const char *)d); break;
            case NVS_TYPE_BLOB: e = nvs_set_blob(h, r.key, d, r.len); break;
            case NVS_TYPE_U8: e = nvs_set_u8(h, r.key, (uint8_t)v); break;
            case NVS_TYPE_I8: e = nvs_set_i8(h, r.key, (int8_t)v); break;
            case NVS_TYPE_U16: e = nvs_set_u16(h, r.key, (uint16_t)v); break;
            case NVS_TYPE_I16: e = nvs_set_i16(h, r.key, (int16_t)v); break;
            case NVS_TYPE_U32: e = nvs_set_u32(h, r.key, (uint32_t)v); break;
            case NVS_TYPE_I32: e = nvs_set_i32(h, r.key, (int32_t)v); break;
            case NVS_TYPE_U64: e = nvs_set_u64(h, r.key, v); break;
            case NVS_TYPE_I64: e = nvs_set_i64(h, r.key, (int64_t)v); break;
            default: e = ESP_ERR_NVS_TYPE_MISMATCH; break;
            }
            if (e == ESP_OK) {
                e = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (e != ESP_OK) {
            failed++;
            ESP_LOGE(TAG, "NVS migration: %s/%s not written (%s)", r.ns, r.key, esp_err_to_name(e));
        }
    }
    return failed;
}

/* Called before nvs_flash_init() of an encrypted build. */
static void nvs_migrate_plain(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_DATA_NVS,
                                                        NVS_DEFAULT_PART_NAME);
    if (p == NULL || nvs_raw_kind(p) != GW_NVSRAW_PLAIN) {
        return;   /* empty or already encrypted: the normal initialisation */
    }
    ESP_LOGW(TAG, "NVS partition \"%s\" is NOT encrypted (written by a build without NVS "
                  "encryption): moving its entries into the encrypted NVS",
             p->label);
    uint8_t *buf = heap_caps_malloc_prefer(p->size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                           MALLOC_CAP_8BIT);
    unsigned count = 0, skipped = 0;
    size_t used = 0;
    uint8_t bdseq = 0;
    bool have_bdseq = false;
    if (nvs_flash_init_partition(NVS_DEFAULT_PART_NAME) == ESP_OK) {
        nvs_handle_t h;
        if (nvs_open_from_partition(NVS_DEFAULT_PART_NAME, NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            have_bdseq = nvs_get_u8(h, NVS_BDSEQ, &bdseq) == ESP_OK;
            nvs_close(h);
        }
        if (buf != NULL) {
            used = nvs_read_all(buf, p->size, &count, &skipped);
        }
        nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
    }
    esp_err_t e = nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME);
    if (e == ESP_OK) {
        e = nvs_flash_init();   /* encrypted (creates the HMAC-derived keys on the first boot) */
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS migration: the encrypted NVS does not start (%s): %u entries lost",
                 esp_err_to_name(e), count);
    } else {
        unsigned failed = buf != NULL ? nvs_write_all(buf, used) : 0;
        if (buf == NULL || failed > 0) {
            /* at least the bdSeq must not go back to a recent value (Sparkplug hosts) */
            nvs_handle_t h;
            uint8_t v = have_bdseq ? bdseq : (uint8_t)(esp_random() & 0xFFu);
            if (nvs_open_from_partition(NVS_DEFAULT_PART_NAME, NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, NVS_BDSEQ, v);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGE(TAG, "NVS migration incomplete (%s, %u entries not written): bdSeq set to %u",
                     buf == NULL ? "no memory" : "write errors", failed, v);
        }
        ESP_LOGW(TAG, "NVS migration: %u entries moved into the encrypted NVS (%u skipped)%s",
                 count - failed, skipped,
                 have_bdseq ? "; bdSeq continues" : "");
        nvs_flash_deinit();   /* gw_storage_init() opens it the normal way */
    }
    if (buf != NULL) {
        memset(buf, 0, p->size);   /* held secrets in clear */
        heap_caps_free(buf);
    }
}
#endif

esp_err_t gw_storage_init(void)
{
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
    mbedtls_platform_set_calloc_free(tls_calloc, heap_caps_free);
#endif
#if CONFIG_NVS_ENCRYPTION
    nvs_migrate_plain();   /* issue #67 */
#endif
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

void *gw_storage_big_alloc(size_t size, const char *what)
{
    /* Not with EXT_RAM_BSS_ATTR: .bss in PSRAM (SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) excludes
     * SPIRAM_IGNORE_NOTFOUND, so a board whose PSRAM is not detected would not boot at all;
     * this way it still runs from internal RAM. */
    void *p = heap_caps_calloc_prefer(1, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p != NULL) {
        ESP_LOGI(TAG, "%s: %u B in %s", what, (unsigned)size,
                 esp_ptr_external_ram(p) ? "PSRAM" : "internal RAM");
    }
    return p;
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

/* ---------- device certificate of the web server (issue #64) ---------- */

bool gw_storage_get_web_tls(char *cert, size_t cert_cap, char *key, size_t key_cap)
{
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(NVS_SECRETS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_str(h, "web_cert", cert, &cert_cap) == ESP_OK &&
              nvs_get_str(h, "web_key", key, &key_cap) == ESP_OK;
    nvs_close(h);
    return ok;
}

esp_err_t gw_storage_set_web_tls(const char *cert, const char *key)
{
    nvs_handle_t h;
    if (!s_nvs_ok) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    esp_err_t err = nvs_open(NVS_SECRETS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "web_cert", cert);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "web_key", key);
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
    uint16_t web_https_port;
    bool web_https;
} lkg_t;

void gw_storage_save_last_good(const gw_config_t *cfg)
{
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(NVS_GW, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    lkg_t now = {.size = sizeof(lkg_t), .web_port = cfg->web.port,
                 .web_https_port = cfg->web.https_port, .web_https = cfg->web.https}, old;
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
        cfg->web.https = lkg.web_https;
        cfg->web.https_port = lkg.web_https_port;
        memset(&lkg, 0, sizeof(lkg));
    } else {
        cfg->network.dhcp = true;
        cfg->network.wifi.failback_s = 30;
        cfg->network.wifi.ipv4.dhcp = true;
        cfg->web.port = 80;
        cfg->web.https = true;
        cfg->web.https_port = 443;
    }
    if (cfg->network.hostname[0] == '\0') {
        gw_hostname_from("", cfg->network.hostname, sizeof(cfg->network.hostname));
        cfg->network.mdns = true;
    }
    strcpy(cfg->ntp.server, "pool.ntp.org");
    cfg->web.enabled = true;
    strcpy(cfg->web.username, "admin");
    ESP_LOGW(TAG, "recovery network: %s", found ? "the last valid configuration's"
                                               : "defaults (DHCP on Ethernet)");
}

static bool mount(bool format_if_needed, esp_err_t *why);

/* Erase the whole LittleFS partition, then format it. esp_littlefs_format only rewrites the
 * superblocks: the data blocks of old files (secrets in clear) would survive. Erased in 64 KiB
 * steps so the other tasks (and the idle task watchdog) keep running. */
static esp_err_t wipe_storage(void)
{
    if (s_mounted) {
        esp_vfs_littlefs_unregister("storage");
        s_mounted = false;
    }
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t off = 0; off < p->size; off += 0x10000) {
        size_t n = p->size - off < 0x10000 ? p->size - off : 0x10000;
        ESP_RETURN_ON_ERROR(esp_partition_erase_range(p, off, n), TAG, "erase");
        vTaskDelay(1);
    }
    return esp_littlefs_format("storage");
}

/* ---------- small local I/O state (IO-3, IO-4) ---------- */

#define BLOB_MAX 64

esp_err_t gw_storage_set_blob(const char *key, const void *data, size_t len)
{
    if (s_reset_done) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    if (!s_nvs_ok || len > BLOB_MAX || nvs_open(NVS_GW, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t old[BLOB_MAX];
    size_t old_len = sizeof(old);
    esp_err_t err = ESP_OK;
    if (nvs_get_blob(h, key, old, &old_len) != ESP_OK || old_len != len ||
        memcmp(old, data, len) != 0) {
        err = nvs_set_blob(h, key, data, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }
    nvs_close(h);
    return err;
}

bool gw_storage_get_blob(const char *key, void *out, size_t len)
{
    nvs_handle_t h;
    if (!s_nvs_ok || len > BLOB_MAX || nvs_open(NVS_GW, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t buf[BLOB_MAX];
    size_t got = sizeof(buf);
    bool ok = nvs_get_blob(h, key, buf, &got) == ESP_OK && got == len;
    nvs_close(h);
    if (ok) {
        memcpy(out, buf, len);
    }
    return ok;
}

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
    ESP_LOGW(TAG, "factory reset: erasing LittleFS 'storage' and NVS");
    s_reset_done = true;
    esp_err_t fs = wipe_storage();
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

bool gw_storage_secrets_encrypted(void)
{
#if CONFIG_NVS_ENCRYPTION
    return s_nvs_ok;
#else
    return false;
#endif
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
    /* Without PSRAM the file and the parser's tree (several times the file) come from the
     * internal heap: log what is available, so a "memoria insuficiente" can be put in context. */
    ESP_LOGI(TAG, CFG_PATH ": %zu bytes; free heap: internal %u B (largest block %u B), PSRAM %u B",
             size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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

static bool ensure_mounted(esp_err_t *why)
{
    if (!s_mounted && !mount(false, why)) {
        /* nothing usable in the partition: saving is the way out of that state */
        ESP_LOGW(TAG, "LittleFS 'storage' not mountable (%s): formatting it to save",
                 esp_err_to_name(*why));
        if (!mount(true, why)) {
            ESP_LOGE(TAG, "LittleFS 'storage' unusable: %s", esp_err_to_name(*why));
            return false;
        }
    }
    return true;
}

/* Temporary file + fsync + rename: a power cut leaves the old or the new file, never a mix
 * (LittleFS renames atomically, replacing the destination). */
static esp_err_t write_file_atomic(const char *path, const char *tmp, const void *data,
                                   size_t len)
{
    esp_err_t why = ESP_OK;
    if (!ensure_mounted(&why)) {
        return why;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s: errno %d", tmp, errno);
        return ESP_FAIL;
    }
    bool ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "writing %s failed (errno %d)", path, errno);
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t write_atomic(const char *text, size_t len)
{
    return write_file_atomic(CFG_PATH, CFG_TMP, text, len);
}

/* ---------- other files next to config.json (TLS certificates, MQTT-4/5) ---------- */

char *gw_storage_read_file(const char *name, size_t *len, char *why, size_t why_cap)
{
    char path[sizeof(CFG_MOUNT) + GW_FILE_SIZE + 1];
    struct stat st;
    if (!gw_config_file_name_ok(name) ||
        snprintf(path, sizeof(path), CFG_MOUNT "/%s", name) >= (int)sizeof(path)) {
        snprintf(why, why_cap, "%s: nombre no válido", name);
        return NULL;
    }
    if (!s_mounted || stat(path, &st) != 0) {
        snprintf(why, why_cap, "%s: no existe en la partición LittleFS", path);
        return NULL;
    }
    if (st.st_size <= 0 || st.st_size > GW_STORAGE_FILE_MAX) {
        snprintf(why, why_cap, "%s: %ld bytes (de 1 a %d)", path, (long)st.st_size,
                 GW_STORAGE_FILE_MAX);
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *buf = boot_alloc(size + 1);
    FILE *f = buf != NULL ? fopen(path, "rb") : NULL;
    size_t got = f != NULL ? fread(buf, 1, size, f) : 0;
    if (f != NULL) {
        fclose(f);
    }
    if (got != size) {
        snprintf(why, why_cap, "%s: %s", path, buf == NULL ? "sin memoria" : "error de lectura");
        heap_caps_free(buf);
        return NULL;
    }
    buf[size] = '\0';
    *len = size;
    return buf;
}

/* The files next to config.json, kept in RAM across a wipe of the partition (CFG-7 migration):
 * only config.json carried secrets in clear; the certificates must survive. */
#define KEEP_MAX 8

typedef struct {
    char name[GW_FILE_SIZE];
    char *data;
    size_t len;
} kept_file_t;

static size_t keep_files(kept_file_t *kept)
{
    size_t n = 0;
    DIR *d = opendir(CFG_MOUNT);
    for (struct dirent *e; d != NULL && (e = readdir(d)) != NULL && n < KEEP_MAX;) {
        if (!gw_config_file_name_ok(e->d_name)) {
            continue;   /* config.json, its temporary file */
        }
        char why[96];
        kept[n].data = gw_storage_read_file(e->d_name, &kept[n].len, why, sizeof(why));
        if (kept[n].data == NULL) {
            ESP_LOGE(TAG, "%s: not kept across the wipe", why);
            continue;
        }
        memcpy(kept[n].name, e->d_name, strlen(e->d_name) + 1);   /* < GW_FILE_SIZE: checked */
        n++;
    }
    if (d != NULL) {
        closedir(d);
    }
    return n;
}

static void restore_files(kept_file_t *kept, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char path[sizeof(CFG_MOUNT) + GW_FILE_SIZE + 1];
        char tmp[sizeof(path) + 4];
        bool fits = snprintf(path, sizeof(path), CFG_MOUNT "/%s", kept[i].name) < (int)sizeof(path) &&
                    snprintf(tmp, sizeof(tmp), "%s.tmp", path) < (int)sizeof(tmp);
        if (!fits || write_file_atomic(path, tmp, kept[i].data, kept[i].len) != ESP_OK) {
            ESP_LOGE(TAG, "%s lost in the wipe", path);
        }
        heap_caps_free(kept[i].data);
    }
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
         * in clear. They are used, moved to NVS and the file is rewritten without them.
         * LittleFS is copy-on-write: the old blocks would keep the secrets until reused, so
         * the partition (which only holds this file) is erased before the rewrite. A power
         * cut in between leaves the device in commissioning mode, never on the example
         * configuration. */
        esp_err_t e = put_secrets(&doc.secrets);
        kept_file_t kept[KEEP_MAX];
        size_t nkept = 0;
        if (e == ESP_OK) {
            set_factory_marker(true);
            nkept = keep_files(kept);
            e = wipe_storage();
        }
        if (e == ESP_OK) {
            e = write_atomic(doc.doc, doc.doc_len);
        }
        restore_files(kept, nkept);
        if (e == ESP_OK) {
            set_factory_marker(false);
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

static size_t s_validation_min_block;

size_t gw_storage_validation_min_block(void)
{
    return s_validation_min_block;
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
    bool applied = gw_cfgdoc_apply(json, len, GW_CFGDOC_UPDATE, &stored, cfg, &doc, err);
    /* issue #80: the new configuration and its document are both allocated here: the deepest
     * point of a validation (the parser's tree is freed already, but its fragments remain) */
    size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_validation_min_block == 0 || block < s_validation_min_block) {
        s_validation_min_block = block;
    }
    ESP_LOGI(TAG, "configuration %s: free heap: internal %u B (largest block %u B, minimum since "
                  "boot %u B)", applied ? "validated" : "rejected",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned)block,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!applied) {
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
