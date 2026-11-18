/*
 * Issue #226: MQTT, as the sparkplug task receives it. A sequence of messages for a node that
 * is in session: NCMD and DCMD through gw_node_handle_message() (the topic too comes from the
 * input in one of the modes), and STATE of the primary host through gw_phost_on_state(). Each
 * payload that the node publishes in reply must decode (spb_decode).
 *
 * Input: a control byte (bit 0: the configuration of the E2E tests, else the example of SPEC
 * section 6; bits 1-7: the devices that stay online, the others go offline after their
 * births), then up to 8 messages: a kind byte (bits 0-1: NCMD, DCMD of the device of bits 2-6,
 * a topic of the input (a length byte, then the topic), STATE; bit 7: the write queue has room
 * for one write only), the length of the payload (2 bytes) and the payload.
 *
 * Each input starts from a copy of the node after its births (s_born), which makes the target
 * 10 times faster than births for each input.
 */
#include "fuzz.h"
#include "gw_node.h"
#include "gw_ota.h"
#include "gw_phost.h"
#include "gw_port.h"
#include "gw_tagdb.h"
#include "spb.h"

static gw_config_t s_cfg[2];
static gw_node_t s_node;
static gw_node_t s_born[2];   /* each configuration: the node after its births */
static gw_tag_value_t s_vals[GW_MAX_TAGS_PER_DEVICE];
static uint8_t s_buf[GW_SPB_PAYLOAD_MAX];
static gw_phost_t s_phost;
static size_t s_room;

static int op_publish(void *ctx, const char *topic, const uint8_t *data, size_t len, int qos,
                      bool retain)
{
    (void)ctx;
    (void)qos;
    (void)retain;
    FZ_CHECK(strncmp(topic, "spBv1.0/", 8) == 0);
    FZ_CHECK(len <= sizeof(s_buf));
    spb_rx_header_t hdr;
    if (!spb_decode(data, len, &hdr, NULL, NULL)) {
        fprintf(stderr, "%s: the published payload does not decode: %s\n", topic,
                spb_last_error());
        abort();
    }
    return 0;
}

static bool op_set_do(void *ctx, uint8_t index, bool on)
{
    (void)ctx;
    FZ_CHECK(index < GW_LOCAL_IO_CHANNELS);
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    uint8_t d = on ? (uint8_t)(io.dout | (1u << index)) : (uint8_t)(io.dout & ~(1u << index));
    gw_tagdb_set_do(d, gw_port_time_ms());
    return true;
}

static bool op_request_write(void *ctx, uint16_t tag, const gw_tag_value_t *value)
{
    (void)ctx;
    (void)value;
    FZ_CHECK(tag < GW_MAX_TAGS);
    if (s_room == 0) {
        return false;
    }
    s_room--;
    return true;
}

static size_t op_write_room(void *ctx)
{
    (void)ctx;
    return s_room;
}

static void op_set_scan_rate(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
}

static void op_reboot(void *ctx)
{
    (void)ctx;
}

static void op_firmware_update(void *ctx, const char *url)
{
    (void)ctx;
    FZ_CHECK(strlen(url) < GW_OTA_URL_MAX);
}

static void op_firmware_sha256(void *ctx, const uint8_t *sha256)
{
    (void)ctx;
    (void)sha256;
}

static void op_boot_other(void *ctx)
{
    (void)ctx;
}

static void op_diagnostics(void *ctx, gw_node_diag_t *out)
{
    (void)ctx;
    *out = (gw_node_diag_t){.uptime_s = 12, .free_heap = 150000, .min_free_heap = 140000,
                            .largest_block = 110592, .free_psram = 8290000, .has_rssi = true};
}

/* Every device of @p cfg online, with valid values. */
static void all_online(const gw_config_t *cfg)
{
    gw_tagdb_reset();
    for (uint16_t d = 0; d < cfg->device_count; d++) {
        gw_tagdb_put_device(cfg, d, s_vals, true);
    }
}

static const gw_node_ops_t k_ops = {
    .publish = op_publish,
    .set_do = op_set_do,
    .request_write = op_request_write,
    .write_room = op_write_room,
    .set_scan_rate = op_set_scan_rate,
    .reboot = op_reboot,
    .firmware_update = op_firmware_update,
    .firmware_sha256 = op_firmware_sha256,
    .boot_other = op_boot_other,
    .diagnostics = op_diagnostics,
};

static const gw_node_info_t k_info = {.firmware = "0.1.0+gdeadbee", .hardware = "fuzz",
                                      .reset_reason = "power on"};

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    const char *paths[2] = {GW_FUZZ_SPEC_EXAMPLE, GW_FUZZ_E2E_CONFIG};
    for (int i = 0; i < 2; i++) {
        size_t len;
        char *json = fz_read_file(paths[i], &len);
        gw_cfg_err_t err;
        if (!gw_config_parse(json, len, &s_cfg[i], &err)) {
            fprintf(stderr, "%s: %s\n", paths[i], err.msg);
            abort();
        }
        free(json);
    }
    gw_port_fake_time(1700000000000ull);
    for (size_t i = 0; i < GW_MAX_TAGS_PER_DEVICE; i++) {
        s_vals[i].valid = true;
        s_vals[i].ts_ms = 1000;
    }
    /* the births once per configuration: each input starts from a copy (fast) */
    for (int i = 0; i < 2; i++) {
        all_online(&s_cfg[i]);
        gw_node_init(&s_born[i], &s_cfg[i], &k_ops, &k_info);
        gw_node_set_buffer(&s_born[i], s_buf, sizeof(s_buf));
        s_room = 64;
        gw_node_session_up(&s_born[i], 0);
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_t f = {data, size};
    uint8_t ctl = fz_u8(&f);
    const gw_config_t *cfg = &s_cfg[ctl & 1u];

    /* the node in session with every device born (a copy of the state after the births), then
     * the devices that the control byte sets offline: their DDEATH */
    all_online(cfg);
    memcpy(&s_node, &s_born[ctl & 1u], sizeof(s_node));
    for (uint16_t d = 0; d < cfg->device_count && d < 7; d++) {
        if (((ctl >> (d + 1)) & 1u) == 0) {
            gw_tagdb_put_device(cfg, d, s_vals, false);
            gw_node_device_changed(&s_node, d);
        }
    }
    gw_phost_init(&s_phost);
    gw_phost_session_start(&s_phost, 0);

    char topic[GW_TOPIC_MAX + 32];
    for (int msg = 0; msg < 8 && f.n != 0; msg++) {
        uint8_t kind = fz_u8(&f);
        s_room = (kind & 0x80u) != 0 ? 1 : 64;
        uint8_t *raw = NULL;   /* a topic of the input, without a NUL (as esp-mqtt gives it) */
        size_t raw_len = 0;
        switch (kind & 3u) {
        case 0:
            gw_node_topic(&s_node, "NCMD", NULL, topic, sizeof(topic));
            break;
        case 1: {
            uint16_t d = cfg->device_count != 0 ? (uint16_t)(((kind >> 2) & 31u) % cfg->device_count)
                                                : 0;
            char id[GW_TOPIC_MAX];
            if (!gw_config_device_id(cfg, d, id, sizeof(id)) ||
                !gw_node_topic(&s_node, "DCMD", id, topic, sizeof(topic))) {
                snprintf(topic, sizeof(topic), "spBv1.0/x/DCMD/y/z");
            }
            break;
        }
        case 2: {
            const uint8_t *t = fz_bytes(&f, fz_u8(&f), &raw_len);
            raw = fz_dup(t, raw_len);
            break;
        }
        default:
            break;
        }
        size_t len;
        const uint8_t *p = fz_bytes(&f, fz_u16(&f), &len);
        uint8_t *payload = fz_dup(p, len);
        if ((kind & 3u) == 3u) {
            (void)gw_phost_on_state(&s_phost, payload, len);
        } else if (raw != NULL) {
            gw_node_handle_message(&s_node, (const char *)raw, raw_len, payload, len);
        } else {
            gw_node_handle_message(&s_node, topic, strlen(topic), payload, len);
        }
        free(payload);
        free(raw);
    }
    return 0;
}
