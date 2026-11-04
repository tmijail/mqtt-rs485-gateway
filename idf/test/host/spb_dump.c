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

/* SPB-19: a DataSet (every column kind) and a File metric, in a standalone NDATA, so the
 * official proto checks the codec's wire format of both. */
static const char *const k_ds_cols[] = {"Device", "Online", "OK", "Total", "Ratio", "Load", "Temp"};
static const uint32_t k_ds_types[] = {SPB_DT_STRING, SPB_DT_BOOLEAN, SPB_DT_UINT32, SPB_DT_UINT64,
                                      SPB_DT_DOUBLE, SPB_DT_FLOAT,   SPB_DT_INT16};

static void ds_cell(const void *ctx, uint32_t row, uint32_t col, spb_value_t *out)
{
    (void)ctx;
    switch (col) {
    case 0: out->str = row == 0 ? "Meter1" : "Pump2"; break;
    case 1: out->num = row == 0; break;
    case 2: out->num = 4000000000.0; break;
    case 3: out->u64 = 9007199254740993ull; break;
    case 4: out->num = 0.25; break;
    case 5: out->num = -1.5; break;
    default: out->num = -5; break;
    }
}

static size_t dataset_payload(uint8_t *out, size_t cap)
{
    static const spb_dataset_t ds = {.columns = 7, .names = k_ds_cols, .types = k_ds_types,
                                     .rows = 2, .cell = ds_cell};
    static const uint8_t file[] = "line 1\nline 2\n";
    const spb_metric_t m[] = {
        {.alias = 900, .datatype = SPB_DT_DATASET, .timestamp = 1762171203000ull, .v.ds = &ds},
        {.alias = 901, .datatype = SPB_DT_FILE, .v.bytes = {file, sizeof(file) - 1}},
    };
    spb_payload_t p = {.timestamp = 1762171203000ull, .has_seq = true, .seq = 9, .metrics = m,
                       .metric_count = 2};
    return spb_encode(&p, out, cap);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output dir>\n", argv[0]);
        return 2;
    }
    const gw_config_t *cfg = support_spec_config();
    gw_tagdb_reset();
    recorder_clear();
    recorder_default_diag();
    g_rec.diag.rssi_valid = true;
    g_rec.diag.rssi = -61;
    gw_node_ops_t ops = recorder_ops();
    static gw_node_t node;
    const gw_node_info_t info = {.firmware = "0.1.0+g0123456",
                                 .hardware = "Waveshare ESP32-S3-POE-ETH-8DI-8DO",
                                 .reset_reason = "power-on"};
    gw_node_init(&node, cfg, &ops, &info);
    support_node_buffer(&node);

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
    /* SPB-16: a failed read is null with Quality BAD */
    v[1].valid = false;
    v[1].ts_ms += 2000;
    gw_tagdb_put_device(cfg, 0, v, true);
    gw_node_device_changed(&node, 0);

    /* MB-6 metrics (Int64, UInt64, Double, String, register bit) in a second node's DBIRTH and
     * DDATA, so the official proto checks their wire fields too. */
    static const char mb6_json[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"Plant1\","
        " \"edge_node_id\": \"GW09\"}, \"devices\": [{\"name\": \"Meter6\", \"unit_id\": 1,"
        " \"tags\": [{\"name\": \"Energy\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint64\","
        " \"units\": \"Wh\", \"eng_low\": 0, \"eng_high\": 1e12},"
        " {\"name\": \"Counter\", \"area\": \"holding\", \"address\": 4, \"type\": \"int64\","
        " \"word_order\": \"CDAB\"},"
        " {\"name\": \"Flow\", \"area\": \"input\", \"address\": 0, \"type\": \"float64\"},"
        " {\"name\": \"Serial\", \"area\": \"holding\", \"address\": 10, \"type\": \"string\","
        " \"length\": 8},"
        " {\"name\": \"Alarm\", \"area\": \"holding\", \"address\": 20, \"type\": \"bool\", \"bit\": 3},"
        " {\"name\": \"Raw\", \"area\": \"holding\", \"address\": 30, \"type\": \"bytes\","
        " \"length\": 3}]}]}";
    static gw_config_t mb6;
    gw_cfg_err_t err;
    if (!gw_config_parse(mb6_json, sizeof(mb6_json) - 1, &mb6, &err)) {
        fprintf(stderr, "MB-6 configuration rejected: %s\n", err.msg);
        return 1;
    }
    /* SPB-18: two devices of one type (template definition in NBIRTH, instances in DBIRTH and
     * DDATA) and an untyped one */
    static const char tmpl_json[] =
        "{\"mqtt\": {\"host\": \"h\"}, \"sparkplug\": {\"group_id\": \"Plant1\","
        " \"edge_node_id\": \"GW08\"}, \"devices\": ["
        " {\"name\": \"PumpA\", \"unit_id\": 1, \"type\": \"Pump\", \"tags\": ["
        "  {\"name\": \"Speed\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\","
        "   \"units\": \"rpm\", \"eng_high\": 3000},"
        "  {\"name\": \"Run\", \"area\": \"coil\", \"address\": 0, \"type\": \"bool\"}]},"
        " {\"name\": \"PumpB\", \"unit_id\": 2, \"type\": \"Pump\", \"tags\": ["
        "  {\"name\": \"Speed\", \"area\": \"holding\", \"address\": 0, \"type\": \"uint16\"},"
        "  {\"name\": \"Run\", \"area\": \"coil\", \"address\": 0, \"type\": \"bool\"}]}]}";
    static gw_config_t tc;
    if (!gw_config_parse(tmpl_json, sizeof(tmpl_json) - 1, &tc, &err)) {
        fprintf(stderr, "template configuration rejected: %s\n", err.msg);
        return 1;
    }
    gw_tagdb_reset();
    static gw_node_t node8;
    gw_node_init(&node8, &tc, &ops, &info);
    support_node_buffer(&node8);
    gw_tag_value_t pv[2] = {{.value = 1450, .ts_ms = 1762171200000ull, .valid = true},
                            {.value = 1, .ts_ms = 1762171200000ull, .valid = true}};
    gw_tagdb_put_device(&tc, 0, pv, true);
    gw_tagdb_put_device(&tc, 1, pv, true);
    gw_node_session_up(&node8, 5);
    pv[0].value = 1500;
    pv[1].valid = false;
    gw_tagdb_put_device(&tc, 0, pv, true);
    gw_node_device_changed(&node8, 0);

    gw_tagdb_reset();
    static gw_node_t node6;
    gw_node_init(&node6, &mb6, &ops, &info);
    support_node_buffer(&node6);
    gw_tag_value_t w[6];
    memset(w, 0, sizeof(w));
    memcpy(w[5].str, "\x00\xff\x10\x00\x7f\x80", 6);   /* SPB-19 Bytes */
    w[0].u64 = 9007199254740993ull;
    w[1].i64 = -9007199254740993ll;
    w[2].value = 1234.56789012345;
    strcpy(w[3].str, "SN-0042-ABCD");
    w[4].value = 1;
    for (int k = 0; k < 6; k++) {
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
    static uint8_t ds_buf[1024];
    size_t dsl = dataset_payload(ds_buf, sizeof(ds_buf));
    snprintf(path, sizeof(path), "%s/99_NDATA.bin", argv[1]);
    f = fopen(path, "wb");
    if (dsl == 0 || f == NULL || fwrite(ds_buf, 1, dsl, f) != dsl) {
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
