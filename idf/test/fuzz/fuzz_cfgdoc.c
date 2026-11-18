/*
 * Issue #226: the configuration document with its secrets (CFG-4, CFG-7). gw_cfgdoc_apply() in
 * both modes (a PUT of the REST API, and the file at boot), gw_cfgdoc_export() (GET of the
 * document, also of a rejected one), and the collector of a document that is typed on the
 * console (gw_jsonfeed_add, in pieces), and gw_cfgprev_diff (the paths that differ from another
 * document, for a downgrade). The allocator can fail at a chosen allocation, as in fuzz_config.
 *
 * Input: a control byte (bits 0-5: the allocation that fails, 0 = none; bit 6: the update mode,
 * else the boot file; bit 7: no secrets are stored), then the document.
 */
#define _GNU_SOURCE   /* memmem */
#include "fuzz.h"
#include "gw_cfgdoc.h"
#include "gw_cfgprev.h"
#include "gw_config.h"

static gw_config_t s_cfg;
static char *s_e2e;   /* tools/e2e/fs/config.json, for gw_cfgprev_diff */
static size_t s_e2e_len;

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    s_e2e = fz_read_file(GW_FUZZ_E2E_CONFIG, &s_e2e_len);
    return 0;
}
static unsigned s_allocs;
static unsigned s_fail_at;

static void *fail_alloc(size_t size)
{
    if (++s_allocs == s_fail_at) {
        return NULL;
    }
    return malloc(size);
}

static bool s_secret_in_input;

static void check_doc(const char *doc, size_t len)
{
    FZ_CHECK(doc != NULL);
    FZ_CHECK(strlen(doc) == len);
    /* a document that leaves the gateway has no stored secret in clear */
    if (!s_secret_in_input) {
        FZ_CHECK(strstr(doc, "mqtt-secret") == NULL);
        FZ_CHECK(strstr(doc, "wifi-secret") == NULL);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t ctl = fz_u8(&f);
    size_t len;
    const uint8_t *rest = fz_rest(&f, &len);
    char *json = (char *)fz_dup(rest, len);

    s_secret_in_input = memmem(json, len, "-secret", 7) != NULL;
    gw_secrets_t stored;
    memset(&stored, 0, sizeof(stored));
    if ((ctl & 128u) == 0) {
        snprintf(stored.mqtt_password, sizeof(stored.mqtt_password), "mqtt-secret");
        snprintf(stored.wifi_password, sizeof(stored.wifi_password), "wifi-secret");
        snprintf(stored.web_password_hash, sizeof(stored.web_password_hash),
                 "pbkdf2-sha256$1$00$00");
        memset(stored.ap_pmk, 'a', sizeof(stored.ap_pmk) - 1);
    }

    s_allocs = 0;
    s_fail_at = ctl & 63u;
    gw_config_set_allocator(fail_alloc, free);
    gw_cfgdoc_mode_t mode = (ctl & 64u) != 0 ? GW_CFGDOC_UPDATE : GW_CFGDOC_FILE;
    gw_cfgdoc_t out;
    gw_cfg_err_t err;
    if (gw_cfgdoc_apply(json, len, mode, &stored, &s_cfg, &out, &err)) {
        check_doc(out.doc, out.doc_len);
        FZ_CHECK(memchr(out.secrets.mqtt_password, 0, sizeof(out.secrets.mqtt_password)));
        FZ_CHECK(memchr(out.secrets.wifi_password, 0, sizeof(out.secrets.wifi_password)));
        FZ_CHECK(memchr(out.secrets.web_password_hash, 0,
                        sizeof(out.secrets.web_password_hash)));
        FZ_CHECK(memchr(out.secrets.ap_pmk, 0, sizeof(out.secrets.ap_pmk)));
        gw_cfgdoc_free(&out);
    } else {
        FZ_CHECK(memchr(err.msg, 0, sizeof(err.msg)) != NULL);
    }

    size_t xlen = 0;
    char *x = gw_cfgdoc_export(json, len, &stored, &xlen, &err);
    if (x != NULL) {
        check_doc(x, xlen);
        gw_cfgdoc_free_text(x);
    }
    /* issue #226: the allocator is global state; the next input starts with the default */
    gw_config_set_allocator(malloc, free);

    /* the paths that differ from a stored document (a downgrade, gw_cfgprev) */
    char diff[200];
    size_t nd = gw_cfgprev_diff(json, len, s_e2e, s_e2e_len, diff, sizeof(diff));
    FZ_CHECK(memchr(diff, 0, sizeof(diff)) != NULL);
    if (nd != (size_t)-1) {
        FZ_CHECK(gw_cfgprev_diff(json, len, json, len, diff, sizeof(diff)) == 0);
    }

    /* the console: the same text in pieces of 1 to 64 bytes */
    char feedbuf[512];
    gw_jsonfeed_t feed;
    gw_jsonfeed_init(&feed, feedbuf, sizeof(feedbuf));
    size_t pos = 0;
    while (pos < len && !feed.complete) {
        size_t piece = 1u + ((unsigned char)json[pos] & 63u);
        if (piece > len - pos) {
            piece = len - pos;
        }
        char *s = fz_strdup((const uint8_t *)json + pos, piece);
        (void)gw_jsonfeed_add(&feed, s);
        free(s);
        pos += piece;
        FZ_CHECK(feed.len < feed.cap);
        FZ_CHECK(feedbuf[feed.len] == '\0');
    }
    free(json);
    return 0;
}
