/*
 * Issue #226: the small JSON bodies of the REST API and of MQTT: POST /api/claim
 * (gw_claim_parse), POST /api/modbus/read (gw_mbdiag_parse, then gw_mbdiag_answer, which must
 * be JSON), POST /api/do (gw_iocmd_parse), POST /api/firmware (gw_ota_parse_request, then
 * gw_ota_url_ok), POST /api/log/level (gw_logring_parse_set), and the STATE of the primary host
 * (gw_phost_parse).
 *
 * Input: a selector byte (bits 0-2: the parser; bits 3-7: the status of a test read), an option
 * byte (the register values of a test read; for a firmware request, bit 0 permits http), then
 * the body.
 */
#include "cJSON.h"
#include "fuzz.h"
#include "gw_claim.h"
#include "gw_iocmd.h"
#include "gw_logring.h"
#include "gw_mbdiag.h"
#include "gw_ota.h"
#include "gw_phost.h"
#include "gw_port.h"

/* Register values with special meanings: zero, the sign bit, NaN and infinity in float32 and
 * float64, ASCII with '"' and '\\' (a string tag), all bits set. */
static const uint16_t k_values[] = {0x0000, 0x8000, 0x7FC0, 0x7F80, 0xFF80, 0x7FF8, 0x7FF0,
                                    0x2241, 0x5C42, 0x0001, 0xFFFF, 0x4142};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t sel = fz_u8(&f);
    uint8_t opt = fz_u8(&f);
    size_t len;
    const uint8_t *rest = fz_rest(&f, &len);
    char *body = (char *)fz_dup(rest, len);   /* no NUL after the body */
    char why[96];
    why[0] = '\0';

    switch (sel & 7u) {
    case 0: {
        char pw[64];
        gw_claim_result_t r = gw_claim_parse(body, len, pw, sizeof(pw));
        FZ_CHECK(gw_claim_text(r) != NULL);
        if (r == GW_CLAIM_OK) {
            FZ_CHECK(strlen(pw) < sizeof(pw));
        }
        break;
    }
    case 1: {
        gw_mbdiag_req_t rq;
        if (gw_mbdiag_parse(body, len, &rq, why, sizeof(why))) {
            FZ_CHECK(rq.count >= 1 && rq.count <= GW_MBDIAG_MAX);
            (void)gw_mbdiag_fc(&rq);
            /* the answer: the registers that the bus gave, and the status of the read */
            uint16_t values[GW_MBDIAG_MAX];
            for (size_t i = 0; i < GW_MBDIAG_MAX; i++) {
                values[i] = k_values[(opt + i) % (sizeof(k_values) / sizeof(k_values[0]))];
            }
            char out[2048];
            mb_status_t st = (mb_status_t)((sel >> 3) % (MB_ERR_ARG + 1));
            FZ_CHECK(gw_mbdiag_answer(&rq, st, opt, values, out, sizeof(out)) != NULL);
            /* the body of the HTTP answer is JSON (NaN and infinity are null) */
            FZ_CHECK(strlen(out) < sizeof(out) - 1);
            gw_port_json_lock();
            cJSON *j = cJSON_Parse(out);
            FZ_CHECK(j != NULL);
            cJSON_Delete(j);
            gw_port_json_unlock();
        } else {
            FZ_CHECK(memchr(why, 0, sizeof(why)) != NULL);
        }
        break;
    }
    case 2: {
        uint8_t index;
        bool on;
        if (gw_iocmd_parse(body, len, &index, &on, why, sizeof(why))) {
            FZ_CHECK(index < 8);
        } else {
            FZ_CHECK(memchr(why, 0, sizeof(why)) != NULL);
        }
        break;
    }
    case 3: {
        char url[GW_OTA_URL_MAX];
        bool force, has_sha;
        uint8_t sha[GW_OTA_SHA256_LEN];
        if (gw_ota_parse_request(body, len, url, &force, sha, &has_sha, why, sizeof(why))) {
            FZ_CHECK(strlen(url) < sizeof(url));
            (void)gw_ota_url_ok(url, (opt & 1u) != 0, why, sizeof(why));
        }
        FZ_CHECK(memchr(why, 0, sizeof(why)) != NULL);
        break;
    }
    case 4: {
        char tag[GW_LOGTAG_SIZE];
        int level;
        bool reset;
        if (gw_logring_parse_set(body, len, GW_LOGLV_DEBUG, tag, &level, &reset, why,
                                 sizeof(why)) && !reset) {
            FZ_CHECK(gw_logtag_valid(tag));
            FZ_CHECK(level >= GW_LOGLV_NONE && level <= GW_LOGLV_DEBUG);
        }
        FZ_CHECK(memchr(why, 0, sizeof(why)) != NULL);
        break;
    }
    default: {
        bool online;
        uint64_t ts;
        (void)gw_phost_parse((const uint8_t *)body, len, &online, &ts);
        break;
    }
    }
    free(body);
    return 0;
}
