/*
 * Issue #226: the microSD card of store & forward (SPB-14, #124). The boot recovery of the spill
 * (gw_sfspill_prepare through gw_sf_step) on a card image of the input, then the replay of what
 * it found: gw_node_flush_history publishes the stored samples as historical DATA messages,
 * which must decode. A card is outside data: anyone can write one.
 *
 * Input: a card image (fuzz_card.h).
 */
#include "fuzz.h"
#include "fuzz_card.h"
#include "gw_node.h"
#include "gw_port.h"
#include "gw_sf.h"
#include "gw_sfspill.h"
#include "gw_tagdb.h"
#include "spb.h"

#define RAM_RECS 64

static gw_config_t s_cfg;
static uint32_t s_hash;
static card_t s_card;
static gw_sfspill_t s_sp;
static uint64_t s_spmem[(16384 + 7) / 8];
static gw_sf_rec_t s_ram[RAM_RECS];
static gw_sf_t s_sf;
static gw_node_t s_node;
static uint8_t s_buf[GW_SPB_PAYLOAD_MAX];
static uint32_t s_nonce;

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;
    size_t len;
    char *json = fz_read_file(GW_FUZZ_SPEC_EXAMPLE, &len);
    gw_cfg_err_t err;
    if (!gw_config_parse(json, len, &s_cfg, &err)) {
        abort();
    }
    free(json);
    s_hash = gw_sfspill_cfg_hash(&s_cfg);
    if (gw_sfspill_mem_size() > sizeof(s_spmem)) {
        abort();
    }
    gw_port_fake_time(1700000000000ull);
    return 0;
}

static uint32_t next_nonce(void)
{
    return ++s_nonce * 0x9E3779B9u;
}

static int op_publish(void *ctx, const char *topic, const uint8_t *data, size_t len, int qos,
                      bool retain)
{
    (void)ctx;
    (void)qos;
    (void)retain;
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
    (void)index;
    (void)on;
    return true;
}

static bool op_request_write(void *ctx, uint16_t tag, const gw_tag_value_t *value)
{
    (void)ctx;
    (void)tag;
    (void)value;
    return true;
}

static size_t op_write_room(void *ctx)
{
    (void)ctx;
    return 64;
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint32_t unit = card_load(&s_card, data, size);
    gw_sfspill_io_t io = card_io(&s_card, unit);
    s_nonce = 0;
    gw_sfspill_init(&s_sp, &io, CARD_EXT_BLOCKS, CARD_EXTENTS, s_hash, s_spmem);
    s_sp.random = next_nonce;
    gw_sf_init(&s_sf, s_ram, RAM_RECS);
    gw_sf_attach_spill(&s_sf, &s_sp);

    /* the boot recovery, one bounded card operation per pass of the loop */
    uint32_t now_ms = 1000;
    for (int i = 0; i < 2000 && s_sp.ok && (s_sp.phase != GW_SFSPILL_READY || s_sp.sb_due);
         i++) {
        now_ms += 100;
        (void)gw_sf_step(&s_sf, now_ms);
    }
    FZ_CHECK(!s_sp.ok || s_sp.phase == GW_SFSPILL_READY);
    FZ_CHECK(s_sp.found != GW_SFSPILL_FOUND_NOT_YET || !s_sp.ok);
    FZ_CHECK(memchr(s_sp.why, 0, sizeof(s_sp.why)) != NULL);
    FZ_CHECK(gw_sfspill_found_text(&s_sp) != NULL);

    /* the replay to a node in session, with its device online */
    gw_tagdb_reset();
    gw_tag_value_t vals[GW_MAX_TAGS_PER_DEVICE];
    memset(vals, 0, sizeof(vals));
    for (size_t i = 0; i < GW_MAX_TAGS_PER_DEVICE; i++) {
        vals[i].valid = true;
        vals[i].ts_ms = 1000;
    }
    gw_tagdb_put_device(&s_cfg, 0, vals, true);
    gw_node_ops_t ops = {.publish = op_publish, .set_do = op_set_do,
                         .request_write = op_request_write, .write_room = op_write_room,
                         .set_scan_rate = op_set_scan_rate, .reboot = op_reboot};
    const gw_node_info_t info = {.firmware = "0.1.0", .hardware = "fuzz", .reset_reason = "-"};
    gw_node_init(&s_node, &s_cfg, &ops, &info);
    gw_node_set_buffer(&s_node, s_buf, sizeof(s_buf));
    gw_node_set_store(&s_node, &s_sf);
    gw_node_session_up(&s_node, 0);
    for (int i = 0; i < 200; i++) {
        now_ms += 100;
        (void)gw_sf_step(&s_sf, now_ms);
        if (gw_node_flush_history(&s_node, 4) == 0 && !gw_sf_busy(&s_sf)) {
            break;
        }
    }

    /* then new samples while offline, more than the RAM ring holds: blocks on that card */
    gw_node_session_down(&s_node);
    for (int i = 0; i < 300 && s_sp.ok; i++) {
        if (i % 10 == 0) {
            gw_sf_begin(&s_sf);
            now_ms += 100;
            (void)gw_sf_step(&s_sf, now_ms);
        }
        gw_sf_push(&s_sf, 0, (uint16_t)(i % 3), i, false, 2000u + (uint64_t)i);
    }
    FZ_CHECK(memchr(s_sp.why, 0, sizeof(s_sp.why)) != NULL);
    return 0;
}
