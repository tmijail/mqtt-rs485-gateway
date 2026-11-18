/*
 * Issue #226: text checks that many inputs use (the configuration, the web UI, MQTT, the log):
 * gw_utf8_valid and gw_utf8_seq, gw_parse_ipv6 (against inet_pton of the C library),
 * gw_hostname_from, gw_config_file_name_ok, gw_logtag_valid and gw_loglevel_parse; and the log
 * lines, which hold text from outside (names, topics, addresses): gw_syslog_parse_line and
 * gw_syslog_format (remote syslog), gw_logring_put, gw_logring_copy and gw_logring_tag (the live
 * log of the web UI), gw_logtags_add.
 *
 * Input: a selector byte (bit 0: a log line, else the checks), then the text.
 */
#include <arpa/inet.h>

#include "fuzz.h"
#include "gw_config.h"
#include "gw_logring.h"
#include "gw_syslog.h"
#include "gw_utf8.h"

static void checks(const char *s, size_t len)
{
    /* gw_utf8_valid() agrees with a walk over gw_utf8_seq() */
    bool walk = true;
    for (size_t i = 0; i < len;) {
        size_t n = gw_utf8_seq(s + i, len - i);
        if (n == 0) {
            walk = false;
            break;
        }
        FZ_CHECK(n <= 4 && n <= len - i);
        i += n;
    }
    FZ_CHECK(gw_utf8_valid(s) == walk);

    uint8_t a[16], b[16];
    bool ours = gw_parse_ipv6(s, a);
    bool libc = inet_pton(AF_INET6, s, b) == 1;
    if (ours) {
        FZ_CHECK(libc && memcmp(a, b, 16) == 0);
    }

    char host[64];
    gw_hostname_from(s, host, sizeof(host));
    size_t hl = strlen(host);
    FZ_CHECK(hl >= 1 && hl <= 63);
    FZ_CHECK(host[0] != '-' && host[hl - 1] != '-');
    for (size_t i = 0; i < hl; i++) {
        char c = host[i];
        FZ_CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-');
        FZ_CHECK(!(c == '-' && host[i + 1] == '-'));
    }
    char small[8];
    gw_hostname_from(s, small, sizeof(small));
    FZ_CHECK(strlen(small) < sizeof(small));

    if (gw_config_file_name_ok(s)) {
        FZ_CHECK(len >= 1 && len < GW_FILE_SIZE && s[0] != '.');
        FZ_CHECK(strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") ==
                 len);
    }
    if (gw_logtag_valid(s)) {
        FZ_CHECK(len >= 1 && len < GW_LOGTAG_SIZE);
    }
    int lv = gw_loglevel_parse(s);
    FZ_CHECK(lv >= -1 && lv <= GW_LOGLV_VERBOSE);
}

static void log_line(const char *s, size_t len)
{
    gw_syslog_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    (void)gw_syslog_parse_line(s, &rec);
    FZ_CHECK(memchr(rec.tag, 0, sizeof(rec.tag)) != NULL);
    FZ_CHECK(memchr(rec.msg, 0, sizeof(rec.msg)) != NULL);
    char out[512];
    size_t n = gw_syslog_format(&rec, s, 7, out, sizeof(out));
    FZ_CHECK(n < sizeof(out) && strlen(out) == n);
    (void)gw_syslog_format_severity(s);

    /* the live log: a small ring, so that the line also pushes out older ones */
    char ring[300];
    gw_logring_t r;
    gw_logring_init(&r, ring, sizeof(ring));
    gw_logring_put(&r, "I (1) main: start", 17);
    gw_logring_put(&r, s, len);
    gw_logring_put(&r, s, len / 2);
    char copy[200];
    uint32_t from, last;
    size_t cn = gw_logring_copy(&r, 0, copy, sizeof(copy), &from, &last);
    FZ_CHECK(cn <= sizeof(copy));
    FZ_CHECK(from <= last || cn == 0);

    const char *tag;
    size_t tag_len;
    if (gw_logring_tag(s, len, &tag, &tag_len)) {
        FZ_CHECK(tag >= s && tag + tag_len <= s + len);
        gw_logtags_t t;
        gw_logtags_init(&t);
        gw_logtag_t *e = gw_logtags_add(&t, tag, tag_len);
        if (e != NULL) {
            FZ_CHECK(strlen(e->name) == tag_len);
            char json[256];
            size_t jn = gw_logtags_json(&t, false, json, sizeof(json));
            FZ_CHECK(jn < sizeof(json));
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t sel = fz_u8(&f);
    size_t len;
    const uint8_t *rest = fz_rest(&f, &len);
    /* the text up to its first NUL, as a C string of exactly that size */
    const uint8_t *nul = memchr(rest, 0, len);
    if (nul != NULL) {
        len = (size_t)(nul - rest);
    }
    char *s = fz_strdup(rest, len);
    if ((sel & 1u) != 0) {
        log_line(s, len);
    } else {
        checks(s, len);
    }
    free(s);
    return 0;
}
