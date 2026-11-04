/*
 * Writes the payloads the edge node produces for the SPEC example configuration
 * (NDEATH, NBIRTH, DBIRTH, DDATA, NDATA) as <dir>/<n>_<TYPE>.bin, so that
 * tools/spb_decode.py can check them with the official proto and Python protobuf.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gw_config.h"
#include "gw_node.h"
#include "gw_tagdb.h"
#include "support.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output dir>\n", argv[0]);
        return 2;
    }
    const gw_config_t *cfg = support_spec_config();
    gw_tagdb_reset();
    recorder_clear();
    gw_node_ops_t ops = recorder_ops();
    static gw_node_t node;
    const gw_node_info_t info = {.firmware = "0.1.0+g0123456",
                                 .hardware = "Waveshare ESP32-S3-POE-ETH-8DI-8DO",
                                 .reset_reason = "power-on"};
    gw_node_init(&node, cfg, &ops, &info);

    gw_tag_value_t v[3] = {{.value = 230.5, .ts_ms = 1762171200000ull, .valid = true},
                           {.value = -12.3, .ts_ms = 1762171200000ull, .valid = true},
                           {.value = 1, .ts_ms = 1762171200000ull, .valid = true}};
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_tagdb_set_di(0x05, 1762171200000ull);

    static uint8_t death[256];
    size_t dl = gw_node_death_payload(42, death, sizeof(death));
    gw_node_session_up(&node, 42);
    v[0].value = 231.0;
    v[0].ts_ms += 1000;
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);
    gw_tagdb_set_di(0x04, 1762171201000ull);
    gw_node_io_changed(&node);

    /* MB-6 metrics (Int64, UInt64, Double, String, register bit) in a second node's DBIRTH and
     * DDATA, so the official proto checks their wire fields too. */
    static const char mb6_json[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"Plant1\","
        " \"edge_node_id\": \"GW09\"}, \"devices\": [{\"name\": \"Meter6\", \"unit_id\": 1,"
        " \"tags\": [{\"name\": \"Energy\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint64\"},"
        " {\"name\": \"Counter\", \"area\": \"holding\", \"address\": 4, \"type\": \"int64\","
        " \"word_order\": \"CDAB\"},"
        " {\"name\": \"Flow\", \"area\": \"input\", \"address\": 0, \"type\": \"float64\"},"
        " {\"name\": \"Serial\", \"area\": \"holding\", \"address\": 10, \"type\": \"string\","
        " \"length\": 8},"
        " {\"name\": \"Alarm\", \"area\": \"holding\", \"address\": 20, \"type\": \"bool\", \"bit\": 3}]}]}";
    static gw_config_t mb6;
    gw_cfg_err_t err;
    if (!gw_config_parse(mb6_json, sizeof(mb6_json) - 1, &mb6, &err)) {
        fprintf(stderr, "MB-6 configuration rejected: %s\n", err.msg);
        return 1;
    }
    gw_tagdb_reset();
    static gw_node_t node6;
    gw_node_init(&node6, &mb6, &ops, &info);
    gw_tag_value_t w[5];
    memset(w, 0, sizeof(w));
    w[0].u64 = 9007199254740993ull;
    w[1].i64 = -9007199254740993ll;
    w[2].value = 1234.56789012345;
    strcpy(w[3].str, "SN-0042-ABCD");
    w[4].value = 1;
    for (int k = 0; k < 5; k++) {
        w[k].ts_ms = 1762171200000ull;
        w[k].valid = true;
    }
    gw_tagdb_put_device(&mb6, 0, w, true);
    gw_node_session_up(&node6, 3);
    w[1].i64 = INT64_MIN;
    strcpy(w[3].str, "SN-0043");
    gw_tagdb_put_device(&mb6, 0, w, true);
    gw_node_device_changed(&node6, 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/0_NDEATH.bin", argv[1]);
    FILE *f = fopen(path, "wb");
    if (f == NULL || fwrite(death, 1, dl, f) != dl) {
        return 1;
    }
    fclose(f);
    for (size_t i = 0; i < g_rec.count; i++) {
        /* third topic level: spBv1.0/<group>/<TYPE>/... */
        const char *type = strchr(strchr(g_rec.msgs[i].topic, '/') + 1, '/') + 1;
        int type_len = (int)(strchr(type, '/') - type);
        snprintf(path, sizeof(path), "%s/%zu_%.*s.bin", argv[1], i + 1, type_len, type);
        f = fopen(path, "wb");
        if (f == NULL || fwrite(g_rec.msgs[i].payload, 1, g_rec.msgs[i].len, f) != g_rec.msgs[i].len) {
            return 1;
        }
        fclose(f);
        printf("%s %s (%zu bytes)\n", path, g_rec.msgs[i].topic, g_rec.msgs[i].len);
    }
    return 0;
}
