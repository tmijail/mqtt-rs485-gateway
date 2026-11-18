/*
 * Issue #226: the records that the firmware reads back from the flash, the RTC memory, the RTC
 * chip and a firmware image: the last-known-good network record (gw_lkg_decode), the record of
 * the last firmware update (gw_fwlast_parse, NVS), the record of the configuration before an
 * update (gw_cfgprev_parse, NVS), the time registers of the PCF85063 (gw_rtc_decode), and the
 * capabilities block of an image (gw_ota_caps_parse, then gw_ota_caps_accept and
 * gw_ota_preflight).
 *
 * Input: a selector byte (bits 0-2: the record), then the record.
 */
#include "fuzz.h"
#include "gw_config.h"
#include "gw_cfgprev.h"
#include "gw_fwlast.h"
#include "gw_lkgnet.h"
#include "gw_ota.h"
#include "gw_rtc.h"

static gw_config_t s_cfg;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t sel = fz_u8(&f);
    size_t len;
    const uint8_t *rest = fz_rest(&f, &len);
    uint8_t *buf = fz_dup(rest, len);
    char text[256];
    text[0] = '\0';

    switch (sel & 7u) {
    case 0: {
        memset(&s_cfg, 0, sizeof(s_cfg));
        if (gw_lkg_decode(buf, len, &s_cfg)) {
            /* a record that decodes encodes again, and that copy decodes */
            uint8_t again[GW_LKG_MAX];
            size_t n = gw_lkg_encode(&s_cfg, again, sizeof(again));
            FZ_CHECK(n != 0);
            FZ_CHECK(gw_lkg_decode(again, n, &s_cfg));
        }
        break;
    }
    case 1: {
        char *s = fz_strdup(buf, len);
        gw_fwlast_t r;
        if (gw_fwlast_parse(s, &r)) {
            FZ_CHECK(memchr(r.version, 0, sizeof(r.version)) != NULL);
            FZ_CHECK(memchr(r.cause, 0, sizeof(r.cause)) != NULL);
            char out[GW_FWLAST_MAX];
            size_t n = gw_fwlast_format(&r, out, sizeof(out));
            FZ_CHECK(n < sizeof(out) && strlen(out) == n);
            gw_fwlast_state(&r, "0.1.0", text, sizeof(text));
            gw_fwlast_boot_t b = {.pending = (sel & 8u) != 0, .running = "0.1.0",
                                  .other_rejected = (sel & 16u) != 0, .reset = "panic"};
            gw_ota_mode_t expected;
            (void)gw_fwlast_on_boot(&r, &b, &expected);
        }
        free(s);
        break;
    }
    case 2: {
        char *s = fz_strdup(buf, len);
        gw_cfgprev_t p;
        if (gw_cfgprev_parse(s, &p)) {
            FZ_CHECK(memchr(p.armed, 0, sizeof(p.armed)) != NULL);
            FZ_CHECK(memchr(p.version, 0, sizeof(p.version)) != NULL);
            char out[GW_CFGPREV_MAX];
            size_t n = gw_cfgprev_format(&p, out, sizeof(out));
            FZ_CHECK(n < sizeof(out) && strlen(out) == n);
            gw_cfgprev_text(&p, text, sizeof(text));
            (void)gw_cfgprev_decide(&p, "0.1.0", 2, 1234);
        }
        free(s);
        break;
    }
    case 3: {
        uint8_t regs[GW_RTC_REGS] = {0};
        memcpy(regs, buf, len < sizeof(regs) ? len : sizeof(regs));
        int64_t epoch;
        if (gw_rtc_decode(regs, &epoch) == GW_RTC_OK) {
            FZ_CHECK(epoch >= GW_RTC_MIN_EPOCH && epoch <= GW_RTC_MAX_EPOCH);
            uint8_t again[GW_RTC_REGS];
            FZ_CHECK(gw_rtc_encode(epoch, again));
            int64_t e2;
            FZ_CHECK(gw_rtc_decode(again, &e2) == GW_RTC_OK && e2 == epoch);
        }
        break;
    }
    default: {
        gw_ota_caps_info_t img;
        gw_ota_caps_parse(buf, len, &img);
        (void)gw_ota_caps_accept(&img, (sel & 8u) != 0, text, sizeof(text));
        gw_ota_caps_info_t own = {.declared = true, .version = GW_OTA_CAPS_VERSION,
                                  .cfg_schema = GW_CONFIG_SCHEMA, .nvs_format = 0xFE01u,
                                  .lfs_disk = 0x00020001u};
        gw_schema_report_t rep;
        gw_config_schema_init(&rep);
        (void)gw_ota_preflight(&own, &img, (sel & 16u) != 0 ? &rep : NULL, text, sizeof(text));
        FZ_CHECK(memchr(text, 0, sizeof(text)) != NULL);
        break;
    }
    }
    free(buf);
    return 0;
}
