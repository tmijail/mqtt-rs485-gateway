#include "gw_storage.h"
#include "gw_pemfile.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_littlefs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_auth.h"
#include "gw_lkgnet.h"
#include "gw_nvsraw.h"
#include "gw_port.h"
#include "gw_sfspill.h"
#include "gw_wpa.h"
#include "mbedtls/platform.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";

#define CFG_MOUNT   "/cfg"
#define CFG_PATH    CFG_MOUNT "/config.json"
#define CFG_TMP     CFG_MOUNT "/config.json.tmp"
#define CFG_PREV    CFG_MOUNT "/config.prev.json"   /* issue #146 */
#define CFG_NEXT    CFG_MOUNT "/config.next.json"   /* issue #146: kept by a restore */
#define NVS_CFG_PREV "cfg_prev"  /* issue #146: its record (gw_cfgprev.h) */
#define MIG_COPY_NAME CFG_MOUNT "/.nvsmig"   /* issue #94: the NVS migration copy */
#define CFG_MAX     CONFIG_GW_CONFIG_MAX_BYTES
#define NVS_SECRETS "secrets"     /* CFG-7: never in the JSON document */
#define NVS_GW      "gw"
#define NVS_LKG     "lkg_net"     /* network of the last valid configuration (recovery) */
#define NVS_FACTORY "factory"     /* set by a factory reset, cleared by the first save (CFG-5) */

extern const char default_config_json_start[] asm("_binary_default_config_json_start");
extern const char default_config_json_end[] asm("_binary_default_config_json_end");

static bool s_nvs_ok;
static bool s_mounted;
static volatile bool s_reset_done;    /* factory reset: no more state writes until the restart */
static SemaphoreHandle_t s_save_lock;   /* one writer at a time: web, shell */
/* #141: gw_sfspill_cfg_hash() of the configuration saved last in this boot */
static portMUX_TYPE s_saved_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_saved;
static uint32_t s_saved_hash;
static StaticSemaphore_t s_save_lock_buf;
/* The largest blob (gw_storage_set_blob): the pulse counters and the run hours, 24 x 8 B (#217) */
#define BLOB_MAX 192
static uint8_t *s_blob_old;   /* PSRAM: the stored blob, for the comparison */
static SemaphoreHandle_t s_blob_lock;
static StaticSemaphore_t s_blob_lock_buf;
static gw_storage_commit_hook_t s_commit_hook;   /* issue #99: gw_fw, before a commit */
static bool s_safe_mode;   /* issue #144 item 4: no NVS migration in safe mode */
/* Issue #144 item 3: the "since" schema report of the stored configuration (PSRAM, allocated at
 * the first load; under the save lock). NULL or not valid: no document was accepted. */
static gw_schema_report_t *s_schema;

#if CONFIG_GW_IO_SIMULATED
/* Issue #146, emulator only: the E2E needs a key that a later image added. In the QEMU build,
 * "network.hostname" counts as the schema after the one of this build (GW_CONFIG_SCHEMA + 1), so
 * a document with it needs an image that declares that schema (the harness rewrites the
 * capabilities block of an image copy). The other keys count as schema 1 here. */
static const gw_since_t k_sim_since[] = {{"network.hostname", NULL, GW_CONFIG_SCHEMA + 1}};
#endif

/* gw_config_schema_init(), or the emulator's table */
static void schema_init(gw_schema_report_t *r)
{
#if CONFIG_GW_IO_SIMULATED
    gw_schema_init(r, k_sim_since, sizeof(k_sim_since) / sizeof(k_sim_since[0]));
#else
    gw_config_schema_init(r);
#endif
}

#if !CONFIG_SPIRAM && !CONFIG_GW_IO_SIMULATED
#error "issue #135: the firmware needs the PSRAM of the board (CONFIG_SPIRAM); only the QEMU build (CONFIG_GW_IO_SIMULATED) runs without it"
#endif

/* Issue #135: the gateway's own buffers go to the PSRAM of the board, and only there: with
 * PSRAM, internal RAM stays for Wi-Fi, lwIP, the task stacks and DMA, and a full PSRAM is an
 * allocation failure, not a silent move into internal RAM. Without PSRAM (the fault mode of a
 * board whose PSRAM was not detected, which starts only the network, the web server and the
 * shell; or the QEMU build without PSRAM, CONFIG_GW_EMU_NO_PSRAM) there is only internal RAM. */
bool gw_storage_psram(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

static uint32_t own_caps(void)
{
    return gw_storage_psram() ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
                              : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
}

void *gw_storage_malloc(size_t size)
{
    return heap_caps_malloc(size, own_caps());
}

void *gw_storage_calloc(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, own_caps());
}

void *gw_storage_realloc(void *p, size_t size)
{
    return heap_caps_realloc(p, size, own_caps());
}

/* Configuration buffers (the file and the parser's document tree, at boot and for every
 * save or export): gw_storage_malloc(). */
/* Issue #91: the largest block a validation asks for after its gw_config_t (the printed
 * document, gw_cfgdoc) and the largest free internal block just before that request. Set
 * and read under the save lock and the JSON lock (issue #106: no other task's cJSON allocates
 * in between). */
static bool s_track;
static size_t s_track_size;
static size_t s_track_room;

static void *boot_alloc(size_t size)
{
    if (s_track && size > s_track_size && size >= 256) {
        s_track_size = size;
        s_track_room = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return gw_storage_malloc(size);
}

#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
/* MQTT-4: a TLS session takes some 40 KB of heap: in PSRAM (#135). Without PSRAM (the fault
 * mode's HTTPS server, the QEMU build without PSRAM) internal RAM. */
static void *tls_calloc(size_t n, size_t size)
{
    return gw_storage_calloc(n, size);
}
#endif

static bool mount(bool format_if_needed, esp_err_t *why);
static esp_err_t write_file_atomic(const char *path, const char *tmp, const void *data,
                                   size_t len);

static bool s_nvs_plain;     /* this boot runs on the unencrypted NVS (issue #94) */
static bool s_plain_trial;   /* ... only because the image is on trial, LittleFS mounted (#104) */
static bool s_nvs_failed;    /* a migration could not finish: no NVS this boot (issue #94) */
static bool s_mig_pending;   /* no NVS this boot, with a copy the confirmed boot applies */
#if CONFIG_NVS_ENCRYPTION
static bool s_clear_markers; /* a normal encrypted boot: drop stale migration markers */
#endif
/* issue #108 M1: no NVS this boot and LittleFS not mountable, with the copy of a migration
 * possibly in it: nothing may format LittleFS */
static bool s_copy_unknown;
static bool s_fs_unknown;   /* issue #109 B-d: LittleFS not mountable and not blank this boot */
static const char *s_nvs_why;   /* why there is no NVS this boot (shell), NULL otherwise */

#if CONFIG_NVS_ENCRYPTION
/* Issue #94: the raw pages of the NVS partition, without initialising it. @p read_ok: false when
 * a page could not be read (or no memory to read it): the scan then does not describe the whole
 * partition. */
static void raw_scan(const esp_partition_t *p, gw_nvsraw_scan_t *scan, bool *read_ok)
{
    gw_nvsraw_scan_init(scan);
    *read_ok = true;
    uint8_t *page = heap_caps_malloc(GW_NVSRAW_PAGE, MALLOC_CAP_8BIT);
    if (p == NULL || page == NULL) {
        heap_caps_free(page);
        *read_ok = p == NULL;   /* no partition: nothing to read */
        return;
    }
    for (uint32_t off = 0; off + GW_NVSRAW_PAGE <= p->size; off += GW_NVSRAW_PAGE) {
        esp_err_t e = esp_partition_read_raw(p, off, page, GW_NVSRAW_PAGE);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "NVS raw page at 0x%lx not readable (%s)", (unsigned long)off,
                     esp_err_to_name(e));
            *read_ok = false;
            continue;
        }
        gw_nvsraw_scan_page(scan, page);
    }
    heap_caps_free(page);
}

static const esp_partition_t *nvs_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
                                    NVS_DEFAULT_PART_NAME);
}
#endif

/* The NVS as this boot uses it: encrypted (CFG-7) unless it stays unencrypted (issue #94). */
static esp_err_t nvs_init_default(void)
{
    return s_nvs_plain ? nvs_flash_init_partition(NVS_DEFAULT_PART_NAME) : nvs_flash_init();
}

#if CONFIG_NVS_ENCRYPTION
/* ---------- issue #67/#94: unencrypted NVS -> encrypted NVS ----------
 * Initialising the encrypted NVS over an unencrypted one loses every entry (secrets,
 * commissioning password, web certificate, I/O state). Before that, the raw pages tell which
 * kind is there (gw_nvsraw, without initialising anything). An unencrypted one is moved into
 * the encrypted NVS, reset-safe (issue #94):
 *   1. the NVS keys are read, or generated (HMAC key burned in eFuse) before anything changes;
 *   2. every entry is read into a copy (gw_nvsmig format) that is written to LittleFS
 *      (MIG_COPY, temporary file + fsync + rename; in clear: flash encryption, FW-3, is not
 *      in main, issue #133);
 *   3. the partition is erased and initialised encrypted, the entries written, read back and
 *      compared, and MIG_DONE written last;
 *   4. the copy is deleted, then MIG_DONE.
 * A reset anywhere in 3 leaves the copy: the next boot starts over from 3 with it (RESUME). A
 * reset between the end of 3 and 4 leaves the copy and MIG_DONE: the copy is only deleted.
 * A new image on trial does not migrate (gw_nvsmig_decide): it runs on the unencrypted NVS, so
 * a rollback finds what the previous image can read, and gw_fw restarts once after confirming
 * it. */
#define MIG_COPY CFG_MOUNT "/.nvsmig"
#define MIG_TMP  CFG_MOUNT "/.nvsmig.tmp"
#define MIG_DONE "nvsmig_ok"          /* in NVS_GW */
#define MIG_BUSY "nvsmig_busy"        /* in NVS_GW: written first, issue #104 */
#define MIG_ERASE GW_NVSMIG_ERASE_KEY /* in NVS_GW of the plain NVS: erase in progress, #109 */

#if CONFIG_GW_IO_SIMULATED
/* Emulator only: the E2E cuts the power at a point of the migration. MIG_CUT holds the points
 * still to cut at ("12": at 1, then at 2 on a later boot); each one is consumed before the hold,
 * so the next boot goes on. 1: right after the erase; 2: after half of the entries. */
#define MIG_CUT CFG_MOUNT "/.nvsmig-cut"
static void sim_cut(char point)
{
    char pts[8] = {0};
    FILE *f = fopen(MIG_CUT, "rb");
    if (f == NULL) {
        return;
    }
    size_t n = fread(pts, 1, sizeof(pts) - 1, f);
    fclose(f);
    while (n > 0 && (pts[n - 1] == '\n' || pts[n - 1] == '\r' || pts[n - 1] == ' ')) {
        n--;
    }
    if (n == 0 || pts[0] != point) {
        return;
    }
    if (n > 1) {
        write_file_atomic(MIG_CUT, MIG_CUT ".tmp", pts + 1, n - 1);
    } else {
        unlink(MIG_CUT);
    }
    ESP_LOGW(TAG, "NVS migration: emulated power cut %c (emulator only): holding", point);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#else
#define sim_cut(point) ((void)0)
#endif

static size_t int_len(uint8_t type)
{
    switch (type) {
    case NVS_TYPE_U8: case NVS_TYPE_I8: return 1;
    case NVS_TYPE_U16: case NVS_TYPE_I16: return 2;
    case NVS_TYPE_U32: case NVS_TYPE_I32: return 4;
    case NVS_TYPE_U64: case NVS_TYPE_I64: return 8;
    default: return 0;
    }
}

/* Integer entry @p key of @p type (little endian into @p v, int_len(type) bytes). */
static esp_err_t get_int(nvs_handle_t h, const char *key, uint8_t type, uint8_t v[8])
{
    uint64_t x = 0;
    esp_err_t e;
    switch (type) {
    case NVS_TYPE_U8: e = nvs_get_u8(h, key, (uint8_t *)&x); break;
    case NVS_TYPE_I8: e = nvs_get_i8(h, key, (int8_t *)&x); break;
    case NVS_TYPE_U16: e = nvs_get_u16(h, key, (uint16_t *)&x); break;
    case NVS_TYPE_I16: e = nvs_get_i16(h, key, (int16_t *)&x); break;
    case NVS_TYPE_U32: e = nvs_get_u32(h, key, (uint32_t *)&x); break;
    case NVS_TYPE_I32: e = nvs_get_i32(h, key, (int32_t *)&x); break;
    case NVS_TYPE_U64: e = nvs_get_u64(h, key, &x); break;
    case NVS_TYPE_I64: e = nvs_get_i64(h, key, (int64_t *)&x); break;
    default: return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    memcpy(v, &x, 8);   /* little endian: the low bytes first */
    return e;
}

/* Every entry of the open, unencrypted NVS appended to the copy in @p buf; @p vcap: the
 * largest value (the partition's size). @p old: the copy of an interrupted migration (NULL if
 * none), which holds the entries whose namespace entry its erase took (#185). */
static size_t nvs_read_all(uint8_t *buf, size_t cap, size_t used, size_t vcap, unsigned *count,
                           unsigned *skipped, bool *listed_all, const uint8_t *old,
                           size_t old_size)
{
    uint8_t *data = gw_storage_malloc(vcap);   /* one value at a time */
    if (data == NULL) {
        ESP_LOGE(TAG, "NVS migration: no memory (%u B) to read the entries", (unsigned)vcap);
    }
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    while (e == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        e = nvs_entry_next(&it);
        if ((strcmp(info.namespace_name, NVS_GW) == 0 &&
             (strcmp(info.key, MIG_DONE) == 0 || strcmp(info.key, MIG_BUSY) == 0)) ||
            strcmp(info.key, MIG_ERASE) == 0) {
            /* a stale marker would end a later migration early. Issue #109 round 3b: the "erase
             * in progress" entry by its key alone: once its namespace's page is erased, NVS lists
             * it under whatever namespace has its index, and copying it fails */
            continue;
        }
        if (gw_nvsmig_orphan_in_copy(old, old_size, info.namespace_name, info.key,
                                     (uint8_t)info.type)) {
            /* issue #185: the interrupted erase took the page of its namespace entry, so NVS
             * lists it with an empty namespace and it cannot be opened; the copy of that
             * migration has it, and adds it below */
            ESP_LOGW(TAG, "NVS migration: /%s has lost its namespace entry (interrupted erase): "
                          "taken from the copy", info.key);
            continue;
        }
        nvs_handle_t h;
        size_t len = 0;
        bool ok = data != NULL && nvs_open_from_partition(NVS_DEFAULT_PART_NAME,
                                                          info.namespace_name, NVS_READONLY,
                                                          &h) == ESP_OK;
        if (ok) {
            uint8_t type = (uint8_t)info.type;
            if (type == NVS_TYPE_STR) {
                len = vcap;
                ok = nvs_get_str(h, info.key, (char *)data, &len) == ESP_OK;
            } else if (type == NVS_TYPE_BLOB) {
                len = vcap;
                ok = nvs_get_blob(h, info.key, data, &len) == ESP_OK;
            } else {
                len = int_len(type);
                ok = len > 0 && get_int(h, info.key, type, data) == ESP_OK;
            }
            nvs_close(h);
        }
        size_t n = ok ? gw_nvsmig_add(buf, cap, used, info.namespace_name, info.key,
                                      (uint8_t)info.type, data, len)
                      : 0;
        if (n != 0) {
            used = n;
            (*count)++;
        } else {
            (*skipped)++;
            ESP_LOGW(TAG, "NVS migration: %s/%s (type 0x%02x) not copied", info.namespace_name,
                     info.key, (unsigned)info.type);
        }
    }
    nvs_release_iterator(it);
    *listed_all = e == ESP_ERR_NVS_NOT_FOUND;   /* the end of the entries, not another error */
    if (!*listed_all) {
        ESP_LOGE(TAG, "NVS migration: listing the entries stopped (%s)", esp_err_to_name(e));
    }
    if (data != NULL) {
        memset(data, 0, vcap);
        heap_caps_free(data);
    }
    return used;
}

static esp_err_t put_rec(nvs_handle_t h, const gw_nvsmig_rec_t *r)
{
    uint64_t v = 0;
    memcpy(&v, r->data, r->len < sizeof(v) ? r->len : sizeof(v));
    switch (r->type) {
    case NVS_TYPE_STR:
        return r->len > 0 && r->data[r->len - 1] == '\0' ? nvs_set_str(h, r->key, (const char *)r->data)
                                                          : ESP_ERR_INVALID_SIZE;
    case NVS_TYPE_BLOB: return nvs_set_blob(h, r->key, r->data, r->len);
    case NVS_TYPE_U8: return nvs_set_u8(h, r->key, (uint8_t)v);
    case NVS_TYPE_I8: return nvs_set_i8(h, r->key, (int8_t)v);
    case NVS_TYPE_U16: return nvs_set_u16(h, r->key, (uint16_t)v);
    case NVS_TYPE_I16: return nvs_set_i16(h, r->key, (int16_t)v);
    case NVS_TYPE_U32: return nvs_set_u32(h, r->key, (uint32_t)v);
    case NVS_TYPE_I32: return nvs_set_i32(h, r->key, (int32_t)v);
    case NVS_TYPE_U64: return nvs_set_u64(h, r->key, v);
    case NVS_TYPE_I64: return nvs_set_i64(h, r->key, (int64_t)v);
    default: return ESP_ERR_NVS_TYPE_MISMATCH;
    }
}

/* The entry as stored equals the record (read back through the encrypted NVS). */
static bool same_rec(nvs_handle_t h, const gw_nvsmig_rec_t *r, uint8_t *tmp)
{
    size_t len = r->len;
    if (r->type == NVS_TYPE_STR) {
        return nvs_get_str(h, r->key, (char *)tmp, &len) == ESP_OK && len == r->len &&
               memcmp(tmp, r->data, len) == 0;
    }
    if (r->type == NVS_TYPE_BLOB) {
        return nvs_get_blob(h, r->key, tmp, &len) == ESP_OK && len == r->len &&
               memcmp(tmp, r->data, len) == 0;
    }
    size_t n = int_len(r->type);
    return n > 0 && n <= r->len && get_int(h, r->key, r->type, tmp) == ESP_OK &&
           memcmp(tmp, r->data, n) == 0;
}

/* Writes (@p verify: compares) every record of the copy; returns the records that failed. */
static unsigned nvs_put_all(const uint8_t *buf, size_t size, unsigned count, bool verify)
{
    gw_nvsmig_rec_t r;
    size_t largest = 8;
    for (size_t off = 0; gw_nvsmig_next(buf, size, &off, &r);) {
        largest = r.len > largest ? r.len : largest;
    }
    uint8_t *tmp = verify ? gw_storage_malloc(largest)
                          : NULL;
    if (verify && tmp == NULL) {
        ESP_LOGE(TAG, "NVS migration: no memory to read the entries back");
        return count ? count : 1;
    }
    unsigned failed = 0, done = 0;
    for (size_t off = 0; gw_nvsmig_next(buf, size, &off, &r);) {
        if (!verify && done == count / 2) {
            sim_cut('2');
        }
        done++;
        nvs_handle_t h;
        esp_err_t e = nvs_open_from_partition(NVS_DEFAULT_PART_NAME, r.ns,
                                              verify ? NVS_READONLY : NVS_READWRITE, &h);
        if (e == ESP_OK) {
            if (verify) {
                e = same_rec(h, &r, tmp) ? ESP_OK : ESP_ERR_INVALID_CRC;
            } else {
                e = put_rec(h, &r);
                if (e == ESP_OK) {
                    e = nvs_commit(h);
                }
            }
            nvs_close(h);
        }
        if (e != ESP_OK) {
            failed++;
            ESP_LOGE(TAG, "NVS migration: %s/%s %s (%s)", r.ns, r.key,
                     verify ? "does not read back" : "not written", esp_err_to_name(e));
        }
    }
    if (tmp != NULL) {
        memset(tmp, 0, largest);
        heap_caps_free(tmp);
    }
    return failed;
}

/* The NVS keys, before anything is erased: read, or generated on the first encrypted boot (the
 * HMAC scheme burns its key in eFuse then). */
static esp_err_t sec_cfg(nvs_sec_cfg_t *cfg)
{
    nvs_sec_scheme_t *scheme = nvs_flash_get_default_security_scheme();
    if (scheme == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = nvs_flash_read_security_cfg_v2(scheme, cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "NVS keys not readable (%s): generating them", esp_err_to_name(e));
        e = nvs_flash_generate_keys_v2(scheme, cfg);
    }
    return e;
}

/* MIG_DONE holds the CRC of the copy it finished: a leftover copy is recognised as that one,
 * never taken for another (issue #94). Issue #104: MIG_BUSY (the same CRC) is written before any
 * entry, so a boot that cannot reach the copy (LittleFS not mountable) recognises a partly
 * written encrypted NVS. @p set false: erase both markers. */
static esp_err_t mark(const char *key, bool set, uint32_t crc)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open_from_partition(NVS_DEFAULT_PART_NAME, NVS_GW, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = set ? nvs_set_u32(h, key, crc) : nvs_erase_key(h, key);
        if (!set && e == ESP_ERR_NVS_NOT_FOUND) {
            e = ESP_OK;
        }
        if (!set) {
            esp_err_t e2 = nvs_erase_key(h, MIG_BUSY);
            e = e != ESP_OK ? e : e2 == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : e2;
        }
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    return e;
}

static esp_err_t done_marker(bool set, uint32_t crc)
{
    return mark(MIG_DONE, set, crc);
}

/* Issue #109: the offset of the raw page holding the "erase in progress" entry, or -1. Round 3b:
 * found by the entry itself (gw_nvsraw_has_erase_marker), not through the "gw" namespace entry,
 * which may be on a page the erase already took. */
static int32_t raw_marker_page(const esp_partition_t *p)
{
    uint8_t *page = heap_caps_malloc(GW_NVSRAW_PAGE, MALLOC_CAP_8BIT);
    if (p == NULL || page == NULL) {
        heap_caps_free(page);
        return -1;
    }
    int32_t at = -1;
    for (uint32_t off = 0; off + GW_NVSRAW_PAGE <= p->size && at < 0; off += GW_NVSRAW_PAGE) {
        if (esp_partition_read_raw(p, off, page, GW_NVSRAW_PAGE) == ESP_OK &&
            gw_nvsraw_has_erase_marker(page)) {
            at = (int32_t)off;
        }
    }
    heap_caps_free(page);
    return at;
}

/* Issue #109 round 3b B1: the entry was written but its page is not found: it goes again, so it
 * cannot later make an intact partition look partly erased. */
static void drop_marker(void)
{
    nvs_handle_t h;
    if (nvs_flash_init_partition(NVS_DEFAULT_PART_NAME) != ESP_OK) {
        return;
    }
    if (nvs_open_from_partition(NVS_DEFAULT_PART_NAME, NVS_GW, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, MIG_ERASE) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
}

/* Issue #109: "PLAIN with keys" is ambiguous: the keys come first, and a migration may stop before
 * the erase (no memory, no copy, an entry it could not copy) and leave the partition intact. So,
 * right before the erase, an "erase in progress" u8 goes into the plain NVS, and the erase takes
 * its page last: while any page is left from a started erase, the entry is there; without it
 * the erase never started. False: the entry could not be written: nothing is erased. */
static bool erase_plain(void)
{
    const esp_partition_t *p = nvs_partition();
    nvs_handle_t h;
    esp_err_t e = nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);
    if (e == ESP_OK) {
        e = nvs_open_from_partition(NVS_DEFAULT_PART_NAME, NVS_GW, NVS_READWRITE, &h);
        if (e == ESP_OK) {
            e = nvs_set_u8(h, MIG_ERASE, GW_NVSMIG_ERASE_VALUE);
            if (e == ESP_OK) {
                e = nvs_commit(h);
            }
            nvs_close(h);
        }
        nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
    }
    int32_t at = e == ESP_OK ? raw_marker_page(p) : -1;
    if (at < 0) {
        ESP_LOGE(TAG, "NVS migration: the \"erase in progress\" entry could not be written or found "
                      "(%s): nothing erased", esp_err_to_name(e));
        if (e == ESP_OK) {
            drop_marker();
        }
        return false;
    }
    uint32_t a = (uint32_t)at;
    e = a > 0 ? esp_partition_erase_range(p, 0, a) : ESP_OK;
    if (e == ESP_OK && a + GW_NVSRAW_PAGE < p->size) {
        e = esp_partition_erase_range(p, a + GW_NVSRAW_PAGE, p->size - a - GW_NVSRAW_PAGE);
    }
    sim_cut('3');   /* every page erased but the entry's */
    if (e == ESP_OK) {
        e = esp_partition_erase_range(p, a, GW_NVSRAW_PAGE);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS migration: erasing the partition failed (%s)", esp_err_to_name(e));
    }
    return e == ESP_OK;
}

/* Step 3 and 4 (above) from the copy in @p buf. False: the copy stays, no NVS this boot.
 * @p plain: the partition holds an unencrypted NVS that starts (a migration that read it, not a
 * resume, nor one whose NVS does not start by layout: issue #109 round 3b, erased whole as before,
 * without the "erase in progress" entry, which is written through it). */
static bool nvs_apply(const nvs_sec_cfg_t *cfg, const uint8_t *buf, size_t size, unsigned count,
                      bool plain)
{
    esp_err_t e = ESP_FAIL;
    if (plain) {
        sim_cut('0');   /* the copy is written, nothing erased yet */
    }
    for (int attempt = 0; attempt < 2 && e != ESP_OK; attempt++) {
        if (plain && attempt == 0) {
            e = erase_plain() ? ESP_OK : ESP_FAIL;   /* issue #109 */
            if (e != ESP_OK) {
                break;   /* the partition is as it was: the copy stays for the next boot */
            }
        } else {
            e = nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME);
        }
        if (e == ESP_OK) {
            sim_cut('1');
            nvs_sec_cfg_t c = *cfg;   /* the init takes a non-const pointer */
            e = nvs_flash_secure_init_partition(NVS_DEFAULT_PART_NAME, &c);
            memset(&c, 0, sizeof(c));
        }
        if (e == ESP_OK) {
            e = mark(MIG_BUSY, true, gw_nvsmig_crc(buf));   /* issue #104: before any entry */
            unsigned failed = e != ESP_OK ? 1 : nvs_put_all(buf, size, count, false);
            if (failed == 0) {
                failed = nvs_put_all(buf, size, count, true);
            }
            e = e != ESP_OK ? e
                : failed == 0 ? done_marker(true, gw_nvsmig_crc(buf)) : ESP_ERR_NVS_INVALID_STATE;
            if (e != ESP_OK) {
                nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
            }
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "NVS migration: writing the encrypted NVS failed (%s)%s",
                     esp_err_to_name(e), attempt == 0 ? ": once more" : "");
        }
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS migration not finished: its copy " MIG_COPY " stays for the next boot; "
                      "no NVS in this one (no stored secrets)");
        s_nvs_failed = true;
        s_mig_pending = true;
        s_nvs_why = "writing the encrypted NVS failed; the migration is retried at the next boot";
        return false;
    }
    if (unlink(MIG_COPY) != 0) {
        ESP_LOGE(TAG, "NVS migration: " MIG_COPY " not deleted (errno %d): the next boot does", errno);
    } else {
        done_marker(false, 0);
    }
    nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);   /* gw_storage_init() opens it */
    return true;
}

/* This boot does not migrate. Without an earlier copy it stays on the unencrypted NVS; with one
 * the partition is partly erased: no NVS at all (issue #94 N2), the copy waits for a boot that
 * can apply it. */
static void no_migration(const uint8_t *old)
{
    if (old == NULL) {
        s_nvs_plain = true;
    } else {
        s_nvs_failed = true;
        s_mig_pending = true;
        s_nvs_why = "an NVS migration is pending (its copy is kept); retried at the next boot";
    }
}

/* MIGRATE: steps 1 to 4. Nothing is erased unless every entry is in the copy, and the copy is in
 * place. @p old: a valid copy of an earlier migration whose erase did not finish (NULL if none):
 * the partition's entries come first, the copy adds what the partition no longer has. */
static void nvs_migrate(const esp_partition_t *p, const uint8_t *old, size_t old_size)
{
    ESP_LOGW(TAG, "NVS partition \"%s\" is NOT encrypted (written by a build without NVS "
                  "encryption): moving its entries into the encrypted NVS%s",
             p->label, old != NULL ? " (with the copy of an interrupted migration)" : "");
    nvs_sec_cfg_t cfg;
    esp_err_t e = sec_cfg(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS migration: no NVS keys (%s): nothing erased this boot",
                 esp_err_to_name(e));
        no_migration(old);
        return;
    }
    /* records take a little more than entries; plus room for the earlier copy's */
    size_t cap = 2 * p->size + GW_NVSMIG_HDR + (old != NULL ? old_size : 0);
    uint8_t *buf = gw_storage_malloc(cap);
    if (buf == NULL) {
        ESP_LOGE(TAG, "NVS migration: no memory (%u B): nothing erased this boot", (unsigned)cap);
        memset(&cfg, 0, sizeof(cfg));
        no_migration(old);
        return;
    }
    size_t used = gw_nvsmig_begin(buf, cap);
    unsigned count = 0;
    gw_nvsmig_read_t rd = {0};
    e = nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);
    rd.init_ok = e == ESP_OK;
    rd.init_layout_error = e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND;
    if (rd.init_ok) {
        used = nvs_read_all(buf, cap, used, p->size, &count, &rd.skipped, &rd.listed_all, old,
                            old_size);
        nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
    } else {
        ESP_LOGE(TAG, "NVS migration: the unencrypted NVS does not start (%s)", esp_err_to_name(e));
    }
    if (old != NULL) {   /* what the partition lost to the interrupted erase */
        gw_nvsmig_rec_t r, mine;
        for (size_t off = 0; gw_nvsmig_next(old, old_size, &off, &r);) {
            if (gw_nvsmig_find(buf, used, r.ns, r.key, &mine)) {
                continue;   /* the partition's entry is the newer one */
            }
            size_t n = gw_nvsmig_add(buf, cap, used, r.ns, r.key, r.type, r.data, r.len);
            if (n == 0) {
                rd.skipped++;
            } else {
                used = n;
                count++;
            }
        }
    }
    if (!gw_nvsmig_may_erase(&rd)) {
        /* issue #94: an entry that is not in the copy would be lost by the erase */
        ESP_LOGE(TAG, "NVS migration: %s: nothing erased this boot",
                 !rd.init_ok ? "the unencrypted NVS could not be read"
                 : !rd.listed_all ? "its entries could not all be listed"
                                  : "some entries could not be copied");
        no_migration(old);
    } else {
        size_t size = gw_nvsmig_seal(buf, used, (uint16_t)rd.skipped);
        e = write_file_atomic(MIG_COPY, MIG_TMP, buf, size);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "NVS migration: no copy in " MIG_COPY " (%s): nothing erased this boot",
                     esp_err_to_name(e));
            no_migration(old);
        } else if (nvs_apply(&cfg, buf, size, count, gw_nvsmig_erase_with_marker(&rd))) {
            ESP_LOGW(TAG, "NVS migration: %u entries moved into the encrypted NVS (%u skipped)",
                     count, rd.skipped);
        }
    }
    memset(&cfg, 0, sizeof(cfg));
    memset(buf, 0, cap);   /* held secrets in clear */
    heap_caps_free(buf);
}

/* RESUME: the partition was erased; the copy of the interrupted migration (@p buf, @p size,
 * checked) is written encrypted. */
static void nvs_resume(gw_nvsraw_kind_t kind, const uint8_t *buf, size_t size, unsigned count,
                       bool legacy)
{
    nvs_sec_cfg_t cfg;
    esp_err_t e = sec_cfg(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS migration: no NVS keys (%s) to finish it: its copy " MIG_COPY
                      " stays; no NVS this boot", esp_err_to_name(e));
        s_nvs_failed = true;
        s_mig_pending = true;
        s_nvs_why = "no NVS keys to finish a pending NVS migration";
        return;
    }
    e = kind == GW_NVSRAW_ENCRYPTED ? nvs_flash_secure_init_partition(NVS_DEFAULT_PART_NAME, &cfg)
                                    : ESP_ERR_NOT_FOUND;
    if (kind == GW_NVSRAW_ENCRYPTED && e != ESP_OK && e != ESP_ERR_NVS_NO_FREE_PAGES &&
        e != ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* N4: it may hold a finished migration newer than the copy: keep both, no NVS */
        ESP_LOGE(TAG, "NVS migration: the encrypted NVS does not start (%s): its copy " MIG_COPY
                      " stays; no NVS this boot", esp_err_to_name(e));
        s_nvs_failed = true;
        s_mig_pending = true;
        s_nvs_why = "the encrypted NVS does not start and an NVS migration copy is pending";
        memset(&cfg, 0, sizeof(cfg));
        return;
    }
    if (kind == GW_NVSRAW_ENCRYPTED && e == ESP_OK) {
        nvs_handle_t h;
        uint32_t done = 0;
        bool marked = false;
        if (nvs_open_from_partition(NVS_DEFAULT_PART_NAME, NVS_GW, NVS_READONLY, &h) == ESP_OK) {
            esp_err_t m = nvs_get_u32(h, MIG_DONE, &done);
            marked = m == ESP_OK;
            uint8_t one = 0;
            if (m == ESP_ERR_NVS_TYPE_MISMATCH && legacy && nvs_get_u8(h, MIG_DONE, &one) == ESP_OK &&
                one == 1) {
                marked = true;   /* N5: ebc2aa4's marker (u8 1) for its version 1 copy */
                done = gw_nvsmig_crc(buf);
            }
            nvs_close(h);
        }
        if (marked && done == gw_nvsmig_crc(buf)) {   /* this very copy was finished */
            ESP_LOGW(TAG, "NVS migration had finished: deleting its leftover copy " MIG_COPY);
            if (unlink(MIG_COPY) == 0) {
                done_marker(false, 0);
            }
            nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
            memset(&cfg, 0, sizeof(cfg));
            return;
        }
        nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
    }
    ESP_LOGW(TAG, "NVS migration was interrupted (%s): writing its copy (%u entries) into the "
                  "encrypted NVS",
             kind == GW_NVSRAW_EMPTY ? "partition erased" : "partition partly written", count);
    if (nvs_apply(&cfg, buf, size, count, false)) {
        ESP_LOGW(TAG, "NVS migration resumed: %u entries moved into the encrypted NVS (%u skipped)",
                 count, (unsigned)gw_nvsmig_skipped(buf));
    }
    memset(&cfg, 0, sizeof(cfg));
}

static bool running_on_trial(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    return run != NULL && esp_ota_get_state_partition(run, &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

static const char *kind_name(gw_nvsraw_kind_t k)
{
    switch (k) {
    case GW_NVSRAW_EMPTY: return "empty";
    case GW_NVSRAW_PLAIN: return "not encrypted";
    case GW_NVSRAW_MIXED: return "not encrypted, with encrypted pages";
    default: return "encrypted";
    }
}

#define MIG_COPY_MAX (1024u * 1024u)   /* larger than the LittleFS partition: never read */

/* The copy of an earlier migration into a new buffer (@p cap: its size, with room for the header
 * of a version 2 copy). Deleted only when it was read whole and does not check (issue #94); one that cannot
 * be read is kept. */
static gw_nvsmig_copy_t copy_load(uint8_t **out, size_t *size, size_t *cap, uint32_t *count,
                                   bool *legacy)
{
    *legacy = false;
    *out = NULL;
    *size = 0;
    struct stat st;
    if (stat(MIG_COPY, &st) != 0) {
        if (errno == ENOENT) {
            return GW_NVSMIG_COPY_NONE;
        }
        ESP_LOGE(TAG, "NVS migration: " MIG_COPY " cannot be looked at (errno %d): kept", errno);
        return GW_NVSMIG_COPY_UNREADABLE;
    }
    size_t n = st.st_size > 0 ? (size_t)st.st_size : 0;
    uint8_t *buf = NULL;
    bool whole = n == 0;   /* an empty file is read whole */
    if (n > 0 && n <= MIG_COPY_MAX) {
        *cap = n + (GW_NVSMIG_HDR - 16u);   /* a v1 header becomes a v2 one */
        buf = gw_storage_malloc(*cap);
        FILE *f = buf != NULL ? fopen(MIG_COPY, "rb") : NULL;
        whole = f != NULL && fread(buf, 1, n, f) == n;
        if (f != NULL) {
            fclose(f);
        }
    }
    if (!whole) {
        ESP_LOGE(TAG, "NVS migration: " MIG_COPY " (%u B) could not be read (%s): kept, it may be "
                      "the only copy of the entries", (unsigned)n,
                 buf == NULL ? "no memory" : "open or read error");
        if (buf != NULL) {
            memset(buf, 0, *cap);
            heap_caps_free(buf);
        }
        return GW_NVSMIG_COPY_UNREADABLE;
    }
    size_t valid = 0;
    gw_nvsmig_copy_t k = n == 0 ? GW_NVSMIG_COPY_BAD
                                : gw_nvsmig_load(buf, n, *cap, &valid, legacy, count);
    if (k != GW_NVSMIG_COPY_VALID) {
        if (k == GW_NVSMIG_COPY_BAD) {
            ESP_LOGE(TAG, "NVS migration: " MIG_COPY " (%u B) is damaged: discarded", (unsigned)n);
            unlink(MIG_COPY);
        } else {
            ESP_LOGE(TAG, "NVS migration: " MIG_COPY " (%u B) is in a format this firmware does "
                          "not know: kept", (unsigned)n);
        }
        if (buf != NULL) {
            memset(buf, 0, *cap);
            heap_caps_free(buf);
        }
        return k;
    }
    if (*legacy) {
        ESP_LOGW(TAG, "NVS migration: " MIG_COPY " is a version 1 copy: converted");
    }
    *out = buf;
    *size = valid;
    return GW_NVSMIG_COPY_VALID;
}

/* Issue #104: the LittleFS partition is erased (its superblock pair, blocks 0 and 1, all 0xFF):
 * no copy can be in it, nor come back. Raw reads: the same answer under flash encryption. */
static bool storage_blank(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (p == NULL) {
        return true;
    }
    uint8_t *b = heap_caps_malloc(GW_NVSRAW_PAGE, MALLOC_CAP_8BIT);
    bool blank = b != NULL;   /* no memory to look: a copy may be there */
    for (uint32_t off = 0; blank && off < 2u * GW_NVSRAW_PAGE && off + GW_NVSRAW_PAGE <= p->size;
         off += GW_NVSRAW_PAGE) {
        blank = esp_partition_read_raw(p, off, b, GW_NVSRAW_PAGE) == ESP_OK;
        for (size_t i = 0; blank && i < GW_NVSRAW_PAGE; i++) {
            blank = b[i] == 0xFF;
        }
    }
    heap_caps_free(b);
    return blank;
}

/* Issue #104: the encrypted NVS holds a migration's start marker and not its end one: it was cut
 * while writing. Read only with keys that already exist (never generated here). */
static bool mig_started(const nvs_sec_cfg_t *cfg)
{
    nvs_sec_cfg_t c = *cfg;
    esp_err_t e = nvs_flash_secure_init_partition(NVS_DEFAULT_PART_NAME, &c);
    memset(&c, 0, sizeof(c));
    if (e != ESP_OK) {
        /* issue #109 B-d: it does not even start, and the copy that may repair it is out of reach:
         * count it as started (no NVS), never let the normal start erase it */
        ESP_LOGE(TAG, "NVS: the encrypted NVS does not start (%s) and LittleFS does not mount",
                 esp_err_to_name(e));
        return true;
    }
    bool busy = false, done = false;
    nvs_handle_t h;
    uint32_t v;
    if (nvs_open_from_partition(NVS_DEFAULT_PART_NAME, NVS_GW, NVS_READONLY, &h) == ESP_OK) {
        busy = nvs_get_u32(h, MIG_BUSY, &v) == ESP_OK;
        done = nvs_get_u32(h, MIG_DONE, &v) == ESP_OK;
        nvs_close(h);
    }
    nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
    return busy && !done;
}

/* Before the NVS is initialised in an encrypted build: what this boot does with it. */
static void nvs_prepare(void)
{
    const esp_partition_t *p = nvs_partition();
    if (p == NULL) {
        return;
    }
    gw_nvsraw_scan_t sc;
    bool raw_ok = true;
    raw_scan(p, &sc, &raw_ok);
    gw_nvsraw_kind_t kind = gw_nvsraw_verdict(&sc);
    ESP_LOGD(TAG, "NVS raw scan: %lu pages (%lu plain, %lu encrypted), %lu written entries, "
                  "%lu plain items, %lu other", (unsigned long)sc.pages,
             (unsigned long)sc.plain_pages, (unsigned long)sc.enc_pages,
             (unsigned long)sc.written, (unsigned long)sc.plain_ok, (unsigned long)sc.bad);
    bool fs = mount(false, NULL);
    gw_nvsmig_copy_t copy = GW_NVSMIG_COPY_NONE;
    uint8_t *buf = NULL;
    size_t size = 0, cap = 0;
    uint32_t count = 0;
    bool legacy = false;
    bool unknown = false, keys = false, busy = false, erase_marker = false;
    if (fs) {
        unlink(MIG_TMP);   /* a copy cut short before its rename: never used */
        copy = copy_load(&buf, &size, &cap, &count, &legacy);
    } else if (!storage_blank()) {
        /* issue #104: a copy may be in the LittleFS that does not mount this boot (#108 B2: also
         * with a plain partition, which may be partly erased) */
        unknown = true;
        nvs_sec_scheme_t *scheme = nvs_flash_get_default_security_scheme();
        nvs_sec_cfg_t k;
        keys = scheme != NULL && nvs_flash_read_security_cfg_v2(scheme, &k) == ESP_OK;
        busy = keys && kind == GW_NVSRAW_ENCRYPTED && mig_started(&k);
        memset(&k, 0, sizeof(k));
        erase_marker = (kind == GW_NVSRAW_PLAIN || kind == GW_NVSRAW_MIXED) &&
                       raw_marker_page(p) >= 0;   /* issue #109 */
        s_fs_unknown = true;
    }
    const gw_nvsmig_state_t state = {.kind = kind, .raw_ok = raw_ok, .copy = copy,
                                     .on_trial = running_on_trial() || s_safe_mode,
                                     .can_copy = fs,
                                     .copy_unknown = unknown, .keys = keys, .mig_busy = busy,
                                     .erase_marker = erase_marker};
    switch (gw_nvsmig_decide(&state)) {
    case GW_NVSMIG_RESUME:
        nvs_resume(kind, buf, size, count, legacy);
        break;
    case GW_NVSMIG_MIGRATE:
        nvs_migrate(p, buf, size);
        break;
    case GW_NVSMIG_STAY_PLAIN:
        s_nvs_plain = true;
        s_plain_trial = state.on_trial && fs && raw_ok;
        ESP_LOGW(TAG, "NVS partition \"%s\" is %s and stays so this boot: %s", p->label,
                 kind_name(kind),
                 !raw_ok ? "a page could not be read, nothing is erased"
                 : s_safe_mode    ? "safe mode (repeated crashes): no NVS migration"
                 : state.on_trial ? "this firmware is on trial (a rollback must find it as the "
                                    "previous image left it); it is encrypted once the firmware "
                                    "is confirmed"
                                  : "LittleFS is not mountable, no place for the reset-safe copy");
        break;
    case GW_NVSMIG_NO_NVS:
        s_nvs_failed = true;
        s_mig_pending = copy == GW_NVSMIG_COPY_VALID;
        s_copy_unknown = unknown && copy == GW_NVSMIG_COPY_NONE;   /* issue #108 M1 */
        s_nvs_why = unknown && copy == GW_NVSMIG_COPY_NONE
                        ? "LittleFS does not mount and the NVS looks like an interrupted migration "
                          "whose copy (" MIG_COPY ") may be in it: nothing is opened, and LittleFS "
                          "is not formatted (no configuration can be saved), until it mounts again "
                          "and the migration finishes from the copy; or a factory reset"
                    : copy == GW_NVSMIG_COPY_UNREADABLE
                        ? "the copy of an NVS migration (" MIG_COPY ") cannot be read: kept, and "
                          "no NVS on any boot until it can, or until a factory reset (BOOT "
                          "pressed 10 s: erases it with the configuration and the secrets)"
                    : !raw_ok ? "an NVS page could not be read: what the partition holds is "
                                "unknown, nothing is touched"
                              : "an NVS migration is pending (its copy is kept): applied at the "
                                "next boot of a confirmed firmware";
        ESP_LOGE(TAG, "NVS left untouched this boot (no NVS: no stored secrets): %s", s_nvs_why);
        break;
    case GW_NVSMIG_OPEN_ENCRYPTED:
    default:
        s_clear_markers = fs && copy == GW_NVSMIG_COPY_NONE;   /* no migration pending */
        if (!raw_ok) {
            ESP_LOGW(TAG, "NVS: a raw page could not be read; the readable ones are encrypted");
        }
        break;
    }
    if (buf != NULL) {
        memset(buf, 0, cap);
        heap_caps_free(buf);
    }
}
#endif

#if CONFIG_NVS_ENCRYPTION
static void nvs_after_open(void);
#endif

void gw_storage_safe_mode(void)
{
    s_safe_mode = true;
}

esp_err_t gw_storage_init(void)
{
#if CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC
    mbedtls_platform_set_calloc_free(tls_calloc, heap_caps_free);
#endif
#if CONFIG_NVS_ENCRYPTION
    nvs_prepare();   /* issues #67, #94 */
#endif
    esp_err_t err = s_nvs_failed ? ESP_ERR_NVS_NOT_INITIALIZED : nvs_init_default();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) && s_fs_unknown) {
        /* issue #109 B-d: LittleFS may hold the migration copy that repairs it: erase nothing */
        ESP_LOGE(TAG, "NVS does not start (%s) and LittleFS does not mount: not erased, no NVS this "
                      "boot", esp_err_to_name(err));
        s_nvs_failed = true;
        s_copy_unknown = true;
        s_nvs_why = "the NVS does not start and LittleFS, which may hold the copy of an NVS "
                    "migration, does not mount: nothing is erased nor formatted";
    } else if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_init_default();
    }
    s_nvs_ok = err == ESP_OK;
#if CONFIG_NVS_ENCRYPTION
    if (s_nvs_ok) {
        nvs_after_open();   /* issue #104 */
    }
#endif
    s_save_lock = xSemaphoreCreateMutexStatic(&s_save_lock_buf);
    s_blob_lock = xSemaphoreCreateMutexStatic(&s_blob_lock_buf);
    s_blob_old = gw_storage_malloc(BLOB_MAX);
    gw_config_set_allocator(boot_alloc, heap_caps_free);
    if (!s_nvs_ok) {
        ESP_LOGE(TAG, "NVS unavailable (%s): no stored secrets", esp_err_to_name(err));
    }
    return err;
}

bool gw_storage_nvs_encryption_pending(void)
{
    return s_nvs_plain || s_mig_pending;
}

bool gw_storage_nvs_migrates_next_boot(void)
{
    return (s_nvs_plain && s_plain_trial) || s_mig_pending;
}

const char *gw_storage_nvs_unavailable_why(void)
{
    return s_nvs_why;
}

#if CONFIG_NVS_ENCRYPTION
/* A normal encrypted boot drops migration markers that a failed cleanup left behind. */
static void nvs_after_open(void)
{
    if (s_clear_markers) {
        done_marker(false, 0);
    }
}
#endif

void *gw_storage_big_alloc(size_t size, const char *what)
{
    /* Not with EXT_RAM_BSS_ATTR: .bss in PSRAM (SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY) excludes
     * SPIRAM_IGNORE_NOTFOUND, so a board whose PSRAM is not detected would not boot at all and
     * could not report the fault (#135: the fault mode in main). */
    void *p = gw_storage_calloc(1, size);
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
    /* issue #187: the WPA2 key of the access point (the PMK of the web password) */
    {"ap_pmk", offsetof(gw_secrets_t, ap_pmk), GW_WPA_PMK_HEX_SIZE},
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

/* NET-5, issue #187: the WPA2 key of the access point is the web password. Only its PMK is kept
 * (gw_wpa.h), calculated here once for each new password, never in the polling or publish path.
 * The salt is the SSID, which comes from the MAC address of the access point. "" when the MAC
 * cannot be read: the access point then uses the commissioning password. */
static void ap_pmk(const char *password, char out[GW_WPA_PMK_HEX_SIZE])
{
    out[0] = '\0';
    uint8_t mac[6];
    char ssid[GW_WPA_SSID_SIZE];
    esp_err_t e = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "no WPA2 key for the access point: MAC address not readable (%s)",
                 esp_err_to_name(e));
        return;
    }
    gw_wpa_ssid(mac, ssid);
    int64_t t0 = esp_timer_get_time();
    if (!gw_wpa_pmk_hex(password, ssid, out)) {
        ESP_LOGW(TAG, "no WPA2 key for the access point: the web password is not a WPA2 "
                      "passphrase");
        return;
    }
    ESP_LOGI(TAG, "WPA2 key of the access point '%s' (PMK of the web password) calculated in "
                  "%lld ms", ssid, (long long)((esp_timer_get_time() - t0) / 1000));
}

/* CFG-3 / NET-5, issue #120: the claim window stores only the hash of the web password, under
 * the key of the configuration secrets ("web_hash"), and (issue #187) its PMK for the access
 * point ("ap_pmk"). The configuration document is not changed: at the next boot a document
 * without web.password takes the stored hash and PMK (gw_cfgdoc FILE mode). */
esp_err_t gw_storage_set_web_password(const char *password, char *hash, size_t cap)
{
    uint8_t salt[GW_AUTH_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    if (!gw_auth_hash_password(password, salt, GW_AUTH_ITERATIONS, hash, cap)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char pmk[GW_WPA_PMK_HEX_SIZE];
    ap_pmk(password, pmk);
    xSemaphoreTake(s_save_lock, portMAX_DELAY);   /* not in the middle of a configuration save */
    nvs_handle_t h;
    esp_err_t err = s_reset_done ? ESP_ERR_INVALID_STATE
                    : !s_nvs_ok  ? ESP_ERR_NVS_NOT_INITIALIZED
                                 : nvs_open(NVS_SECRETS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "web_hash", hash);
        if (err == ESP_OK && pmk[0] != '\0') {
            err = nvs_set_str(h, "ap_pmk", pmk);
        } else if (err == ESP_OK) {
            err = nvs_erase_key(h, "ap_pmk");
            err = err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_save_lock);
    memset(pmk, 0, sizeof(pmk));
    if (err != ESP_OK) {
        hash[0] = '\0';
    }
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

/* ---------- network of the last valid configuration (CFG-3 recovery) ----------
 * Issue #144 item 8: records that an earlier or a later image reads (gw_lkgnet.h), not a raw
 * struct: a rollback into recovery mode keeps the static IP of the site. The raw struct of the
 * firmware before #144 is not read (no gateway is installed yet): it counts as no record. */

void gw_storage_save_last_good(const gw_config_t *cfg)
{
    nvs_handle_t h;
    if (s_reset_done || !s_nvs_ok || nvs_open(NVS_GW, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint8_t now[GW_LKG_MAX], old[GW_LKG_MAX];
    size_t n = gw_lkg_encode(cfg, now, sizeof(now));
    size_t len = sizeof(old);
    /* written only when it changed: NVS wear */
    if (n > 0 && (nvs_get_blob(h, NVS_LKG, old, &len) != ESP_OK || len != n ||
                  memcmp(old, now, n) != 0)) {
        if (nvs_set_blob(h, NVS_LKG, now, n) != ESP_OK || nvs_commit(h) != ESP_OK) {
            ESP_LOGW(TAG, "network of the valid configuration not kept for recovery");
        }
    }
    nvs_close(h);
    memset(now, 0, sizeof(now));   /* the Wi-Fi password */
    memset(old, 0, sizeof(old));
}

void gw_storage_recovery_config(gw_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    /* the defaults: what a field that the record does not have keeps */
    cfg->network.dhcp = true;
    cfg->network.wifi.failback_s = 30;
    cfg->network.wifi.ipv4.dhcp = true;
    cfg->web.port = 80;
    cfg->web.https = true;
    cfg->web.https_port = 443;
    uint8_t lkg[GW_LKG_MAX];
    size_t len = sizeof(lkg);
    nvs_handle_t h;
    bool found = false;
    if (s_nvs_ok && nvs_open(NVS_GW, NVS_READONLY, &h) == ESP_OK) {
        found = nvs_get_blob(h, NVS_LKG, lkg, &len) == ESP_OK && gw_lkg_decode(lkg, len, cfg);
        nvs_close(h);
    }
    memset(lkg, 0, sizeof(lkg));
    if (cfg->network.hostname[0] == '\0') {
        gw_hostname_from("", cfg->network.hostname, sizeof(cfg->network.hostname));
        cfg->network.mdns = true;
    }
    strcpy(cfg->ntp.servers[0], "pool.ntp.org");
    cfg->ntp.server_count = 1;
    cfg->web.enabled = true;
    strcpy(cfg->web.username, "admin");
    ESP_LOGW(TAG, "recovery network: %s", found ? "the last valid configuration's"
                                               : "defaults (DHCP on Ethernet)");
}

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

/* ---------- small local I/O state (IO-3, IO-4, #217) ---------- */

/* The copy of the stored blob for the comparison is in PSRAM (s_blob_old, allocated at
 * gw_storage_init), not on the stack of the caller: the save of the counters runs also in a
 * shutdown handler (any task, for example the esp_timer task of a restart). */
esp_err_t gw_storage_set_blob(const char *key, const void *data, size_t len)
{
    if (s_reset_done) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_blob_old == NULL) {
        return ESP_ERR_NO_MEM;
    }
    nvs_handle_t h;
    if (!s_nvs_ok || len > BLOB_MAX || nvs_open(NVS_GW, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_blob_lock, portMAX_DELAY);
    size_t old_len = BLOB_MAX;
    esp_err_t err = ESP_OK;
    if (nvs_get_blob(h, key, s_blob_old, &old_len) != ESP_OK || old_len != len ||
        memcmp(s_blob_old, data, len) != 0) {
        err = nvs_set_blob(h, key, data, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }
    xSemaphoreGive(s_blob_lock);
    nvs_close(h);
    return err;
}

esp_err_t gw_storage_set_str(const char *key, const char *value)
{
    if (s_reset_done) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(NVS_GW, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    char old[GW_STORAGE_STR_MAX];
    size_t old_len = sizeof(old);
    esp_err_t err = ESP_OK;
    if (nvs_get_str(h, key, old, &old_len) != ESP_OK || strcmp(old, value) != 0) {
        err = nvs_set_str(h, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }
    nvs_close(h);
    return err;
}

bool gw_storage_get_str(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(NVS_GW, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    bool ok = nvs_get_str(h, key, out, &len) == ESP_OK;
    nvs_close(h);
    return ok;
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
    char why[120];
    esp_err_t h = s_commit_hook != NULL
                      ? s_commit_hook(GW_STORAGE_COMMIT_FACTORY_RESET, why, sizeof(why))
                      : ESP_OK;
    if (h == ESP_ERR_INVALID_STATE) {
        /* refused by the commit hook: nothing erased */
        ESP_LOGE(TAG, "factory reset refused: %s", why);
        return h;
    }
    if (h != ESP_OK) {
        ESP_LOGE(TAG, "factory reset: %s; going on (the restart may roll it back)", why);
    }
    xSemaphoreTake(s_save_lock, portMAX_DELAY);   /* no save in the middle; never given back */
    ESP_LOGW(TAG, "factory reset: erasing LittleFS 'storage' and NVS");
    s_reset_done = true;
    esp_err_t fs = wipe_storage();
    nvs_flash_deinit();
    esp_err_t nv = nvs_flash_erase();
    if (nv == ESP_OK) {
        nv = nvs_init_default();   /* unencrypted while a new firmware is on trial (#94) */
    }
    s_nvs_ok = nv == ESP_OK;
    if (nv == ESP_OK) {
        set_factory_marker(true);   /* next boot: commissioning, not the embedded example */
    }
    ESP_LOGW(TAG, "factory reset: storage %s, NVS %s", esp_err_to_name(fs), esp_err_to_name(nv));
    return fs != ESP_OK ? fs : nv;
}

void gw_storage_set_commit_hook(gw_storage_commit_hook_t hook)
{
    s_commit_hook = hook;
}

bool gw_storage_nvs_encrypted_on_flash(void)
{
#if CONFIG_NVS_ENCRYPTION
    return !s_nvs_plain;
#else
    return false;
#endif
}

bool gw_storage_secrets_encrypted(void)
{
#if CONFIG_NVS_ENCRYPTION
    return s_nvs_ok && !s_nvs_plain;
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
        snprintf(err->msg, sizeof(err->msg), CFG_PATH ": empty file");
        return NULL;
    }
    if (st->st_size > CFG_MAX) {
        snprintf(err->msg, sizeof(err->msg),
                 CFG_PATH ": %ld bytes; the maximum is %d (CONFIG_GW_CONFIG_MAX_BYTES)",
                 (long)st->st_size, CFG_MAX);
        return NULL;
    }
    size_t size = (size_t)st->st_size;
    /* The file and the parser's tree (several times the file) come from the PSRAM (internal
     * heap in the QEMU build without PSRAM): log what is available, so a "not enough
     * memory" can be put in context. */
    ESP_LOGI(TAG, CFG_PATH ": %zu bytes; free heap: internal %u B (largest block %u B), PSRAM %u B",
             size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    char *buf = boot_alloc(size + 1);
    if (buf == NULL) {
        snprintf(err->msg, sizeof(err->msg), CFG_PATH ": no memory to read %zu bytes", size);
        return NULL;
    }
    FILE *f = fopen(CFG_PATH, "rb");
    size_t got = f != NULL ? fread(buf, 1, size, f) : 0;
    int read_errno = errno;
    if (f != NULL) {
        fclose(f);
    }
    if (got != size) {
        snprintf(err->msg, sizeof(err->msg), CFG_PATH ": read error (%zu of %zu bytes, errno %d)",
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
    if (!s_mounted && s_copy_unknown) {
        /* issue #108 M1: the copy of an interrupted NVS migration may be in the partition */
        ESP_LOGE(TAG, "LittleFS 'storage' not mountable, and it may hold the copy of an NVS "
                      "migration (" MIG_COPY_NAME "): not formatted");
        *why = ESP_ERR_INVALID_STATE;
        return false;
    }
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
        snprintf(why, why_cap, "%s: invalid name", name);
        return NULL;
    }
    if (!s_mounted || stat(path, &st) != 0) {
        snprintf(why, why_cap, "%s: not in the LittleFS partition", path);
        return NULL;
    }
    if (st.st_size <= 0 || st.st_size > GW_STORAGE_FILE_MAX) {
        snprintf(why, why_cap, "%s: %ld bytes (1 to %d)", path, (long)st.st_size,
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
        snprintf(why, why_cap, "%s: %s", path, buf == NULL ? "no memory" : "read error");
        heap_caps_free(buf);
        return NULL;
    }
    buf[size] = '\0';
    *len = size;
    return buf;
}

/* #221: a file next to config.json that the operator can load (a PEM file): the name rules of
 * gw_config_file_name_ok(), and not one of the configuration files of the gateway
 * (config.prev.json, config.next.json) or a temporary file. */
bool gw_storage_user_file_name_ok(const char *name)
{
    size_t n = strlen(name);
    return gw_config_file_name_ok(name) && !(n >= 5 && strcmp(name + n - 5, ".json") == 0) &&
           !(n >= 4 && strcmp(name + n - 4, ".tmp") == 0);
}

size_t gw_storage_list_files(void (*cb)(void *ctx, const char *name, size_t size), void *ctx)
{
    size_t n = 0;
    DIR *d = s_mounted ? opendir(CFG_MOUNT) : NULL;
    for (struct dirent *e; d != NULL && (e = readdir(d)) != NULL;) {
        char path[sizeof(CFG_MOUNT) + GW_FILE_SIZE + 1];
        struct stat st;
        if (!gw_storage_user_file_name_ok(e->d_name) ||
            snprintf(path, sizeof(path), CFG_MOUNT "/%s", e->d_name) >= (int)sizeof(path) ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        cb(ctx, e->d_name, (size_t)st.st_size);
        n++;
    }
    if (d != NULL) {
        closedir(d);
    }
    return n;
}

esp_err_t gw_storage_write_file(const char *name, const void *data, size_t len, bool replace,
                                char *why, size_t why_cap)
{
    char path[sizeof(CFG_MOUNT) + GW_FILE_SIZE + 1];
    char tmp[sizeof(path) + 4];
    struct stat st;
    if (!gw_storage_user_file_name_ok(name) ||
        snprintf(path, sizeof(path), CFG_MOUNT "/%s", name) >= (int)sizeof(path) ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        snprintf(why, why_cap, "'%s' is not a valid file name (1-%d characters A-Z a-z 0-9 . _ -, "
                 "not .json)", name, GW_FILE_SIZE - 1);
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0 || len > GW_STORAGE_FILE_MAX) {
        snprintf(why, why_cap, "%u bytes (1 to %d)", (unsigned)len, GW_STORAGE_FILE_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    esp_err_t r = ESP_OK;
    if (!replace && s_mounted && stat(path, &st) == 0) {
        snprintf(why, why_cap, "/cfg/%s exists", name);
        r = ESP_ERR_INVALID_STATE;
    } else if ((r = write_file_atomic(path, tmp, data, len)) != ESP_OK) {
        snprintf(why, why_cap, "could not write /cfg/%s (%s): the LittleFS partition can be full",
                 name, esp_err_to_name(r));
    } else {
        ESP_LOGI(TAG, "/cfg/%s written (%u B)", name, (unsigned)len);
    }
    xSemaphoreGive(s_save_lock);
    return r;
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
        reject(err, CFG_PATH ": cannot get its status (errno %d)", errno);
        return NULL;
    }
    *from_file = false;
    /* EMBED_TXTFILES appends a NUL; strnlen excludes it. */
    size_t n = strnlen(default_config_json_start,
                       (size_t)(default_config_json_end - default_config_json_start));
    char *copy = boot_alloc(n + 1);
    if (copy == NULL) {
        reject(err, "no memory for the embedded configuration");
        return NULL;
    }
    memcpy(copy, default_config_json_start, n + 1);
    *len = n;
    return copy;
}

/* CFG-7: the web password is only ever stored as a salted PBKDF2 hash, and (issue #187) as the
 * PMK of the access point, both in @p sec. */
static bool hash_web_password(gw_config_t *cfg, gw_secrets_t *sec)
{
    uint8_t salt[GW_AUTH_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    bool ok = gw_auth_hash_password(cfg->web.password, salt, GW_AUTH_ITERATIONS,
                                    sec->web_password_hash, sizeof(sec->web_password_hash));
    ap_pmk(cfg->web.password, sec->ap_pmk);
    memset(cfg->web.password, 0, sizeof(cfg->web.password));
    if (ok) {
        strcpy(cfg->web.password_hash, sec->web_password_hash);
    }
    return ok;
}

/* #169: at most this many warnings in the boot log; the dry run and `config check` give all */
#define BOOT_LINT_LINES 16

static unsigned s_boot_lint_lines;

static bool boot_lint(void *ctx, gw_lint_level_t level, const char *path, const char *msg)
{
    (void)ctx;
    (void)level;   /* a GW_LINT_ERROR never gets here: the parse refused it */
    ESP_LOGW(TAG, "configuration warning: %s: %s", path, msg);
    return ++s_boot_lint_lines < BOOT_LINT_LINES;
}

bool gw_storage_load_config(gw_config_t *cfg, gw_cfg_err_t *err, const char **source)
{
    *source = CFG_PATH " (LittleFS)";
    esp_err_t why;
    if (!mount(false, &why)) {
        return reject(err, "LittleFS partition 'storage' not mountable (%s)", esp_err_to_name(why));
    }
#if CONFIG_GW_IO_SIMULATED
    /* issue #144, emulator only: a new image on trial that cannot read the stored configuration,
     * as an image of an older schema (the E2E puts /cfg/.sim-reject-config there). An image that
     * is not on trial reads the configuration as usual */
    struct stat sim;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t run_state;
    if (stat(CFG_MOUNT "/.sim-reject-config", &sim) == 0 && run != NULL &&
        esp_ota_get_state_partition(run, &run_state) == ESP_OK &&
        run_state == ESP_OTA_IMG_PENDING_VERIFY) {
        reject(err, "e2e.simulated: rejected by the emulator (/cfg/.sim-reject-config, a new "
                    "firmware on trial)");
        snprintf(err->path, sizeof(err->path), "e2e.simulated");
        return false;
    }
#endif
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
    if (s_schema == NULL) {
        s_schema = gw_storage_malloc(sizeof(*s_schema));   /* boot time, once */
    }
    bool ok;
    if (s_schema != NULL) {
        schema_init(s_schema);
        ok = gw_cfgdoc_apply_schema(text, len, GW_CFGDOC_FILE, &stored, cfg, &doc, err, s_schema);
    } else {
        ok = gw_cfgdoc_apply(text, len, GW_CFGDOC_FILE, &stored, cfg, &doc, err);
    }
    heap_caps_free(text);
    if (!ok) {
        return false;
    }
    if (doc.new_web_password) {
        hash_web_password(cfg, &doc.secrets);
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
    /* #169: the names that UMH Core and other UNS hosts change, in the boot log */
    s_boot_lint_lines = 0;
    size_t warnings = gw_config_lint(cfg, boot_lint, NULL);
    if (warnings > 0) {
        ESP_LOGW(TAG, "configuration: %u warning(s) for UNS hosts such as UMH Core (#169)%s",
                 (unsigned)warnings, warnings > BOOT_LINT_LINES ? "; the first ones above" : "");
    }
    return true;
}

/* ---------- issue #146: the configuration from before an update ---------- */

#define COPY_CHUNK 1024

static bool cfgprev_load(gw_cfgprev_t *p)
{
    char text[GW_CFGPREV_MAX];
    if (!gw_storage_get_str(NVS_CFG_PREV, text, sizeof(text))) {
        memset(p, 0, sizeof(*p));
        return false;
    }
    return gw_cfgprev_parse(text, p);
}

static void cfgprev_store(const gw_cfgprev_t *p)
{
    char text[GW_CFGPREV_MAX];
    gw_cfgprev_format(p, text, sizeof(text));
    esp_err_t e = gw_storage_set_str(NVS_CFG_PREV, text);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "record of the configuration from before the update not written (%s)",
                 esp_err_to_name(e));
    }
}

/* gw_cfgprev_hash() of a file, read in pieces (no buffer of its size). */
static bool file_hash(const char *path, uint32_t *hash)
{
    char *buf = gw_storage_malloc(COPY_CHUNK);
    FILE *f = buf != NULL ? fopen(path, "rb") : NULL;
    if (f == NULL) {
        heap_caps_free(buf);
        return false;
    }
    uint32_t h = GW_CFGPREV_HASH_INIT;
    size_t n;
    while ((n = fread(buf, 1, COPY_CHUNK, f)) > 0) {
        h = gw_cfgprev_hash(h, buf, n);
    }
    bool ok = ferror(f) == 0;
    fclose(f);
    heap_caps_free(buf);
    *hash = h;
    return ok;
}

/* @p src to @p dst through @p tmp (fsync, then rename: a power cut leaves the old or the new
 * file), in pieces. */
static esp_err_t copy_file_atomic(const char *src, const char *dst, const char *tmp)
{
    esp_err_t why = ESP_OK;
    if (!ensure_mounted(&why)) {
        return why;
    }
    char *buf = gw_storage_malloc(COPY_CHUNK);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    FILE *in = fopen(src, "rb");
    FILE *out = in != NULL ? fopen(tmp, "wb") : NULL;
    bool ok = out != NULL;
    size_t n;
    while (ok && (n = fread(buf, 1, COPY_CHUNK, in)) > 0) {
        ok = fwrite(buf, 1, n, out) == n;
    }
    ok = ok && ferror(in) == 0 && fflush(out) == 0 && fsync(fileno(out)) == 0;
    if (out != NULL) {
        ok = fclose(out) == 0 && ok;
    }
    if (in != NULL) {
        fclose(in);
    }
    heap_caps_free(buf);
    if (!ok || rename(tmp, dst) != 0) {
        ESP_LOGE(TAG, "copy of %s to %s failed (errno %d)", src, dst, errno);
        unlink(tmp);
        return in == NULL ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    return ESP_OK;
}

/* A whole file in a boot_alloc buffer (NUL terminated), NULL if it cannot be read. */
static char *read_whole(const char *path, size_t *len)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > CFG_MAX) {
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
        heap_caps_free(buf);
        return NULL;
    }
    buf[size] = '\0';
    *len = size;
    return buf;
}

/* Under the save lock, before a new document @p doc is written: the first change after an
 * update keeps the stored document as config.prev.json (gw_cfgprev_on_save). */
static void cfgprev_on_save(const char *doc, size_t len)
{
    gw_cfgprev_t p;
    if (!cfgprev_load(&p) || p.armed[0] == '\0') {
        return;
    }
    uint32_t old_hash = 0;
    bool old_ok = s_schema != NULL && s_schema->valid && file_hash(CFG_PATH, &old_hash);
    char armed[GW_CFGPREV_VERSION];
    memcpy(armed, p.armed, sizeof(armed));
    switch (gw_cfgprev_on_save(&p, old_ok, old_ok ? s_schema->need : 0, old_hash,
                               gw_cfgprev_hash(GW_CFGPREV_HASH_INIT, doc, len),
                               (int64_t)time(NULL))) {
    case GW_CFGPREV_CAPTURE:
        if (copy_file_atomic(CFG_PATH, CFG_PREV, CFG_PREV ".tmp") != ESP_OK) {
            ESP_LOGE(TAG, "the configuration from before the update to this firmware is not kept");
            return;   /* the record stays armed */
        }
        cfgprev_store(&p);
        ESP_LOGW(TAG, "first change after the update: the configuration of %s (schema %u) kept as "
                      CFG_PREV, p.version, (unsigned)p.schema);
        break;
    case GW_CFGPREV_DISARM:
        cfgprev_store(&p);
        ESP_LOGW(TAG, "the stored configuration is not one that this firmware accepted: no "
                      "configuration kept for %s", armed);
        break;
    default:
        break;
    }
}

bool gw_storage_cfgprev_get(gw_cfgprev_t *out)
{
    return cfgprev_load(out);
}

void gw_storage_cfgprev_arm(const char *other, const char *running)
{
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    gw_cfgprev_t p;
    cfgprev_load(&p);
    if (gw_cfgprev_arm(&p, other, running)) {
        cfgprev_store(&p);
        ESP_LOGI(TAG, "the first change of the configuration keeps the configuration of %s",
                 other);
    }
    xSemaphoreGive(s_save_lock);
}

bool gw_storage_config_hash(uint32_t *hash)
{
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    bool ok = s_mounted && file_hash(CFG_PATH, hash);
    xSemaphoreGive(s_save_lock);
    return ok;
}

size_t gw_storage_cfgprev_diff(char *out, size_t cap)
{
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    size_t alen = 0, blen = 0;
    char *a = s_mounted ? read_whole(CFG_PREV, &alen) : NULL;
    char *b = a != NULL ? read_whole(CFG_PATH, &blen) : NULL;
    xSemaphoreGive(s_save_lock);
    size_t n = (size_t)-1;
    if (b == NULL) {
        snprintf(out, cap, "the documents cannot be read");
    } else {
        n = gw_cfgprev_diff(a, alen, b, blen, out, cap);
    }
    heap_caps_free(a);
    heap_caps_free(b);
    return n;
}

/* config.prev.json becomes config.json; config.json is kept as config.next.json. Under the save
 * lock. */
static esp_err_t cfgprev_swap(void)
{
    struct stat st;
    if (stat(CFG_PATH, &st) == 0) {
        esp_err_t e = copy_file_atomic(CFG_PATH, CFG_NEXT, CFG_NEXT ".tmp");
        if (e != ESP_OK) {
            return e;
        }
    }
    esp_err_t e = copy_file_atomic(CFG_PREV, CFG_PATH, CFG_TMP);
    if (e == ESP_OK) {
        gw_cfgprev_t p;
        if (cfgprev_load(&p) && p.armed[0] != '\0') {
            p.armed[0] = '\0';   /* the stored document is the earlier one again */
            cfgprev_store(&p);
        }
    }
    return e;
}

esp_err_t gw_storage_cfgprev_restore(uint32_t hash, char *why, size_t cap)
{
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    uint32_t now = 0;
    struct stat st;
    esp_err_t e = ESP_OK;
    if (!s_mounted || stat(CFG_PREV, &st) != 0) {
        e = ESP_ERR_NOT_FOUND;
        snprintf(why, cap, CFG_PREV " is not there");
    } else if (!file_hash(CFG_PATH, &now) || now != hash) {
        e = ESP_ERR_INVALID_STATE;
        snprintf(why, cap, "the configuration changed during the update: not restored");
    } else if ((e = cfgprev_swap()) != ESP_OK) {
        snprintf(why, cap, "the configuration from before the update was not restored (%s)",
                 esp_err_to_name(e));
    }
    xSemaphoreGive(s_save_lock);
    if (e == ESP_OK) {
        ESP_LOGW(TAG, CFG_PREV " restored as " CFG_PATH " for the earlier firmware (the "
                      "configuration of this firmware stays in " CFG_NEXT ")");
    }
    return e;
}

esp_err_t gw_storage_cfgprev_undo(void)
{
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    esp_err_t e = copy_file_atomic(CFG_NEXT, CFG_PATH, CFG_TMP);
    xSemaphoreGive(s_save_lock);
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "the earlier firmware does not boot: " CFG_PATH " of this firmware is back");
    } else {
        ESP_LOGE(TAG, CFG_PATH " of this firmware not put back (%s): it holds the configuration "
                      "from before the update", esp_err_to_name(e));
    }
    return e;
}

esp_err_t gw_storage_cfgprev_restore_checked(gw_cfg_err_t *err)
{
    memset(err, 0, sizeof(*err));
    char hook_why[120];
    if (s_commit_hook != NULL &&
        s_commit_hook(GW_STORAGE_COMMIT_CONFIG, hook_why, sizeof(hook_why)) != ESP_OK) {
        reject(err, "configuration not restored: %s", hook_why);
        return ESP_ERR_INVALID_STATE;
    }
    gw_config_t *cfg = boot_alloc(sizeof(*cfg));
    if (cfg == NULL) {
        reject(err, "no memory to validate the configuration");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    gw_cfgprev_t p;
    memset(&p, 0, sizeof(p));
    size_t len = 0;
    char *text = s_mounted && cfgprev_load(&p) && p.version[0] != '\0'
                     ? read_whole(CFG_PREV, &len) : NULL;
    esp_err_t ret = ESP_OK;
    if (text == NULL) {
        ret = ESP_ERR_NOT_FOUND;
        reject(err, "there is no configuration from before an update");
    } else {
        /* as at the boot: the secrets come from NVS (the documents do not hold them) */
        gw_secrets_t stored;
        gw_storage_get_secrets(&stored);
        gw_cfgdoc_t doc;
        bool ok = gw_cfgdoc_apply(text, len, GW_CFGDOC_FILE, &stored, cfg, &doc, err);
        memset(&stored, 0, sizeof(stored));
        if (ok) {
            memset(&doc.secrets, 0, sizeof(doc.secrets));
            gw_cfgdoc_free(&doc);
            ret = cfgprev_swap();
            if (ret != ESP_OK) {
                reject(err, "could not write " CFG_PATH " (%s)", esp_err_to_name(ret));
            }
        } else {
            ret = strstr(err->msg, "not enough memory") ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
        }
    }
    xSemaphoreGive(s_save_lock);
    heap_caps_free(text);
    memset(cfg, 0, sizeof(*cfg));
    heap_caps_free(cfg);
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "the configuration from before the update (%s) restored as " CFG_PATH,
                 p.version);
    }
    return ret;
}

static size_t s_validation_min_block;

size_t gw_storage_validation_min_block(void)
{
    return s_validation_min_block;
}

#if CONFIG_GW_IO_SIMULATED
static volatile uint32_t s_sim_hold_ms;

void gw_storage_sim_hold_validation(uint32_t ms)
{
    s_sim_hold_ms = ms;
}

#endif

bool gw_storage_saved_cfg_hash(uint32_t *hash)
{
    portENTER_CRITICAL(&s_saved_mux);
    bool saved = s_saved;
    *hash = s_saved_hash;
    portEXIT_CRITICAL(&s_saved_mux);
    return saved;
}

esp_err_t gw_storage_save_config(const char *json, size_t len, bool dry_run, gw_cfg_err_t *err)
{
    return gw_storage_save_config_ex(json, len, dry_run, false, err);
}

esp_err_t gw_storage_save_config_ex(const char *json, size_t len, bool dry_run, bool discard_copy,
                                    gw_cfg_err_t *err)
{
    return gw_storage_save_config_lint(json, len, dry_run, discard_copy, err, NULL, NULL, NULL);
}

esp_err_t gw_storage_save_config_lint(const char *json, size_t len, bool dry_run,
                                      bool discard_copy, gw_cfg_err_t *err, gw_lint_cb_t lint,
                                      void *ctx, size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    memset(err, 0, sizeof(*err));
    if (len > CFG_MAX) {
        reject(err, "%zu bytes; the maximum is %d (CONFIG_GW_CONFIG_MAX_BYTES)", len, CFG_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    gw_config_t *cfg = boot_alloc(sizeof(*cfg));
    if (cfg == NULL) {
        reject(err, "no memory to validate the configuration");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    gw_secrets_t stored;
    gw_storage_get_secrets(&stored);
    gw_cfgdoc_t doc;
    esp_err_t ret = ESP_OK;
    /* issue #144 item 3: the schema report of the new document replaces the stored one's when
     * it is saved */
    gw_schema_report_t *schema = dry_run ? NULL : gw_storage_malloc(sizeof(*schema));
    if (schema != NULL) {
        schema_init(schema);
    }
    gw_port_json_lock();   /* recursive: gw_cfgdoc_apply takes it too */
    s_track_size = s_track_room = 0;
    s_track = true;
    bool applied = schema != NULL ? gw_cfgdoc_apply_schema(json, len, GW_CFGDOC_UPDATE, &stored,
                                                           cfg, &doc, err, schema)
                                  : gw_cfgdoc_apply(json, len, GW_CFGDOC_UPDATE, &stored, cfg,
                                                    &doc, err);
    s_track = false;
    gw_port_json_unlock();
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
    /* issue #91: the print asks for one block of the document's size (twice it before) */
    ESP_LOGI(TAG, "validation: document %u B, largest request %u B, largest internal block "
                  "before it %u B", (unsigned)doc.doc_len, (unsigned)s_track_size,
             (unsigned)s_track_room);
#if CONFIG_GW_IO_SIMULATED
    uint32_t hold = s_sim_hold_ms;
    s_sim_hold_ms = 0;
    if (hold > 0) {
        ESP_LOGW(TAG, "emulator: validation held for %u ms with its buffers", (unsigned)hold);
        vTaskDelay(pdMS_TO_TICKS(hold));
        block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (block < s_validation_min_block) {
            s_validation_min_block = block;
        }
        ESP_LOGW(TAG, "emulator: validation released: free heap: internal %u B (largest block %u B, "
                      "minimum since boot %u B)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)block,
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
#endif
    char hook_why[120];
    if (!applied) {
        ret = strstr(err->msg, "not enough memory") ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    } else if (!gw_pem_check_config(cfg, err)) {
        /* #221: the PEM files that the document names exist, and the key agrees with the
         * certificate (the web UI shows the error at its field) */
        ret = ESP_ERR_INVALID_ARG;
    } else if (!dry_run && s_copy_unknown && !s_mounted && !discard_copy) {
        /* issue #108 M1: formatting LittleFS to save would destroy the only copy */
        ret = ESP_ERR_INVALID_STATE;
        reject(err, "configuration not saved: LittleFS does not mount and can hold the copy of an "
                    "NVS migration; a format would lose it (to discard it: README 6.2.1)");
    } else if (!dry_run && s_commit_hook != NULL &&
               s_commit_hook(GW_STORAGE_COMMIT_CONFIG, hook_why, sizeof(hook_why)) != ESP_OK) {
        /* issue #144 item 1: during the trial of a new image (the restart that applies it would
         * roll the new firmware back) */
        ret = ESP_ERR_INVALID_STATE;
        reject(err, "configuration not saved: %s", hook_why);
    } else if (!dry_run) {
        bool discarded = false;
        if (discard_copy && s_copy_unknown && !s_mounted) {
            /* issue #109: the operator's explicit way out: LittleFS is formatted, the copy is lost */
            ESP_LOGW(TAG, "configuration saved discarding the unreachable NVS migration copy: "
                          "formatting LittleFS (what only the copy had is lost)");
            s_copy_unknown = false;
            discarded = true;
        }
        if (doc.new_web_password &&
            !hash_web_password(cfg, &doc.secrets)) {
            ret = ESP_FAIL;
        }
        if (ret == ESP_OK) {
            cfgprev_on_save(doc.doc, doc.doc_len);   /* issue #146 */
        }
        /* secrets first: if the file cannot be written, the previous ones are put back */
        if (ret == ESP_OK && doc.secrets_changed && (ret = put_secrets(&doc.secrets)) != ESP_OK) {
            reject(err, "could not save the secrets in NVS (%s)", esp_err_to_name(ret));
        }
        if (ret == ESP_OK && (ret = write_atomic(doc.doc, doc.doc_len)) != ESP_OK) {
            reject(err, "could not write " CFG_PATH " (%s)", esp_err_to_name(ret));
            if (doc.secrets_changed) {
                put_secrets(&stored);
            }
        }
        if (ret == ESP_OK) {
            set_factory_marker(false);
            uint32_t hash = gw_sfspill_cfg_hash(cfg);   /* #141: for the restart */
            portENTER_CRITICAL(&s_saved_mux);
            s_saved = true;
            s_saved_hash = hash;
            portEXIT_CRITICAL(&s_saved_mux);
            ESP_LOGI(TAG, "configuration saved to " CFG_PATH " (%zu bytes)%s", doc.doc_len,
                     doc.secrets_changed ? "; secrets updated in NVS" : "");
            if (schema != NULL && schema->valid) {
                if (s_schema == NULL) {
                    s_schema = schema;   /* no report before (recovery, commissioning) */
                    schema = NULL;
                } else {
                    *s_schema = *schema;
                }
            }
        }
        if (ret != ESP_OK && discarded && !s_mounted) {
            s_copy_unknown = true;   /* nothing was formatted after all */
        }
    }
    gw_cfgdoc_free(&doc);
    xSemaphoreGive(s_save_lock);
    heap_caps_free(schema);
    /* #169: after the document is freed (the callback may send its findings to a client) */
    if (ret == ESP_OK && lint != NULL) {
        size_t n = gw_config_lint(cfg, lint, ctx);
        if (count != NULL) {
            *count = n;
        }
    }
    memset(cfg, 0, sizeof(*cfg));   /* it held the secrets in clear */
    heap_caps_free(cfg);
    return ret;
}

bool gw_storage_config_schema(gw_schema_report_t *out)
{
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    bool ok = s_schema != NULL && s_schema->valid;
    if (ok) {
        *out = *s_schema;
    }
    xSemaphoreGive(s_save_lock);
    return ok;
}

char *gw_storage_export_config(size_t *len, gw_cfg_err_t *err)
{
    memset(err, 0, sizeof(*err));
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    char *out = NULL;
    esp_err_t why;
    if (!mount(false, &why)) {
        reject(err, "LittleFS partition 'storage' not mountable (%s)", esp_err_to_name(why));
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
