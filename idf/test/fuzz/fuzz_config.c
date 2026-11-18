/*
 * Issue #226: the web UI and the REST API (PUT /api/config, the shell's "config set"), and the
 * configuration file in LittleFS. gw_config_parse() and gw_config_parse_schema() on a document
 * of the input, with an allocator that fails at a chosen allocation (the parser's tree is in
 * PSRAM in the firmware, and the PSRAM can be full). On a valid document: what the firmware
 * derives from it (gw_config_lint, the device ids, the mDNS name, the hash of the spill, the
 * last-known-good network record of gw_lkgnet), and gw_filerefs_parse and gw_json_depth on the
 * same text.
 *
 * Input: a control byte (bits 0-5: the allocation that fails, 0 = none; bit 6: the variant with
 * the schema report), then the document.
 */
#include "fuzz.h"
#include "gw_certs.h"
#include "gw_config.h"
#include "gw_lkgnet.h"
#include "gw_sfspill.h"

static gw_config_t s_cfg;
static gw_config_t s_back;
static unsigned s_allocs;
static unsigned s_fail_at;

/* The allocator of the parser: allocation number s_fail_at gives NULL. */
static void *fail_alloc(size_t size)
{
    if (++s_allocs == s_fail_at) {
        return NULL;
    }
    return malloc(size);
}

static bool lint_cb(void *ctx, gw_lint_level_t level, const char *path, const char *msg)
{
    size_t *n = ctx;
    (void)level;
    *n += strlen(path) + strlen(msg);
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t ctl = fz_u8(&f);
    size_t len;
    const uint8_t *rest = fz_rest(&f, &len);
    char *json = (char *)fz_dup(rest, len);   /* no NUL after the document */

    s_allocs = 0;
    s_fail_at = ctl & 63u;
    gw_config_set_allocator(fail_alloc, free);
    gw_cfg_err_t err;
    bool ok;
    if ((ctl & 64u) != 0) {
        gw_schema_report_t rep;
        gw_config_schema_init(&rep);
        ok = gw_config_parse_schema(json, len, &s_cfg, &err, &rep);
        FZ_CHECK(rep.valid == ok);
    } else {
        ok = gw_config_parse(json, len, &s_cfg, &err);
    }
    /* issue #226: the allocator is global state; the next input starts with the default */
    gw_config_set_allocator(malloc, free);
    FZ_CHECK(memchr(err.msg, 0, sizeof(err.msg)) != NULL);

    gw_filerefs_t refs;
    (void)gw_filerefs_parse(json, len, &refs);
    (void)gw_json_depth(json, len);

    if (ok) {
        size_t n = 0;
        (void)gw_config_lint(&s_cfg, lint_cb, &n);
        for (uint16_t d = 0; d < s_cfg.device_count; d++) {
            char id[GW_TOPIC_SIZE];
            FZ_CHECK(gw_config_device_id(&s_cfg, d, id, sizeof(id)));
        }
        char inst[GW_MDNS_INSTANCE_SIZE];
        gw_config_mdns_instance(&s_cfg, inst, sizeof(inst));
        (void)gw_sfspill_cfg_hash(&s_cfg);
        uint8_t rec[GW_LKG_MAX];
        size_t rl = gw_lkg_encode(&s_cfg, rec, sizeof(rec));
        FZ_CHECK(rl != 0);
        memset(&s_back, 0, sizeof(s_back));
        FZ_CHECK(gw_lkg_decode(rec, rl, &s_back));
    }
    free(json);
    return 0;
}
