#include "gw_node.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gw_conv.h"
#include "gw_mbap.h"
#include "gw_port.h"

static const char *TAG = "spb";

static const char *const k_di_names[GW_NODE_IO_COUNT] = {"DI/1", "DI/2", "DI/3", "DI/4",
                                                         "DI/5", "DI/6", "DI/7", "DI/8"};
static const char *const k_do_names[GW_NODE_IO_COUNT] = {"DO/1", "DO/2", "DO/3", "DO/4",
                                                         "DO/5", "DO/6", "DO/7", "DO/8"};
static const char *const k_count_names[GW_NODE_IO_COUNT] = {
    "Counter/1", "Counter/2", "Counter/3", "Counter/4",
    "Counter/5", "Counter/6", "Counter/7", "Counter/8"};
#define NAME_BDSEQ    "bdSeq"
#define NAME_REBIRTH  "Node Control/Rebirth"
#define NAME_REBOOT   "Node Control/Reboot"
#define NAME_FIRMWARE "Node Info/Firmware"
#define NAME_HARDWARE "Node Info/Hardware"
#define NAME_RESET_REASON "Node Info/Reset Reason"
#define NAME_SCAN_RATE "Node Control/Scan Rate"
#define NAME_FW_UPDATE "Node Control/Firmware Update"
#define NAME_FW_STATE "Firmware Update/State"
#define NAME_FW_PROGRESS "Firmware Update/Progress"
#define NAME_UPTIME   "Node Info/Uptime"
#define NAME_FREE_HEAP "Node Info/Free Heap"
#define NAME_MIN_HEAP "Node Info/Min Free Heap"
#define NAME_RSSI     "Node Info/RSSI"
#define NAME_MODBUS   "Node Info/Modbus"
#define NAME_TIME_SYNCED "Node Info/Time Synced"
#define NAME_MODBUS_TCP "Node Info/Modbus TCP"

/* Issue #81: "Node Info/Modbus TCP", the Modbus TCP gateway's forwarded requests per device
 * (gw_dev_stats_t.tcp, #69) and a last row for units with no device. A table of its own rather
 * than a transport column in "Node Info/Modbus": that table keeps its columns for the hosts
 * that read it, the poll columns (cycles) mean nothing for forwarded requests, and this one
 * exists only when the gateway is enabled. */
static const char *const k_tcp_cols[] = {"Device", "Unit", "Online", "OK", "Timeout", "CRC",
                                         "Frame", "Exception"};
static const uint32_t k_tcp_types[] = {SPB_DT_STRING, SPB_DT_UINT32, SPB_DT_BOOLEAN,
                                       SPB_DT_UINT32, SPB_DT_UINT32,  SPB_DT_UINT32,
                                       SPB_DT_UINT32, SPB_DT_UINT32};
#define OTHER_UNITS "(other units)"

/* SPB-17 / MB-13: "Node Info/Modbus", one row per device. */
static const char *const k_mb_cols[] = {"Device", "Online", "Cycles", "Failed Cycles", "OK",
                                        "Timeout", "CRC", "Frame", "Exception"};
static const uint32_t k_mb_types[] = {SPB_DT_STRING, SPB_DT_BOOLEAN, SPB_DT_UINT32,
                                      SPB_DT_UINT32, SPB_DT_UINT32,  SPB_DT_UINT32,
                                      SPB_DT_UINT32, SPB_DT_UINT32,  SPB_DT_UINT32};

/* ---------- helpers ---------- */

void gw_node_init(gw_node_t *n, const gw_config_t *cfg, const gw_node_ops_t *ops,
                  const gw_node_info_t *info)
{
    memset(n, 0, sizeof(*n));
    n->cfg = cfg;
    n->ops = *ops;
    n->info = *info;
    strcpy(n->fw_state, "idle");
}

void gw_node_set_buffer(gw_node_t *n, uint8_t *buf, size_t cap)
{
    n->buf = buf;
    n->buf_cap = cap;
}

bool gw_node_topic(const gw_node_t *n, const char *type, const char *device, char *out, size_t cap)
{
    return spb_topic(out, cap, n->cfg->sparkplug.group_id, type, n->cfg->sparkplug.edge_node_id,
                     device);
}

static uint8_t next_seq(gw_node_t *n)
{
    return n->seq++;  /* uint8_t wraps 255 -> 0 (SPB-5) */
}

/* Returns 0, GW_NODE_PUBLISH_TOO_LARGE or -1 (any other failure). */
static int publish_rc(gw_node_t *n, const char *type, const char *device, size_t metric_count,
                      int qos)
{
    spb_payload_t p = {
        .timestamp = gw_port_time_ms(),
        .has_seq = true,
        .seq = next_seq(n),
        .metrics = n->metrics,
        .metric_count = metric_count,
    };
    size_t len = n->buf != NULL ? spb_encode(&p, n->buf, n->buf_cap) : 0;
    if (len == 0 || !gw_node_topic(n, type, device, n->topic, sizeof(n->topic))) {
        GW_LOGE(TAG, "%s%s%s: payload does not fit", type, device ? " " : "", device ? device : "");
        return -1;
    }
    int r = n->ops.publish(n->ops.ctx, n->topic, n->buf, len, qos, false);
    if (r < 0) {
        if (r != GW_NODE_PUBLISH_TOO_LARGE) {
            GW_LOGW(TAG, "publish %s failed", n->topic);
            return -1;
        }
        /* never sent (too large for the broker, #73): its seq goes to the next message, or
         * hosts would see a gap in a session that lost nothing (SPB-5) */
        n->seq = (uint8_t)p.seq;
        return r;
    }
    GW_LOGD(TAG, "%s seq=%" PRIu64 " metrics=%u bytes=%u", n->topic, p.seq, (unsigned)metric_count,
            (unsigned)len);
    return 0;
}

static bool publish(gw_node_t *n, const char *type, const char *device, size_t metric_count,
                    int qos)
{
    return publish_rc(n, type, device, metric_count, qos) == 0;
}

static spb_metric_t *metric(gw_node_t *n, size_t i, const char *name, uint64_t alias,
                            uint32_t datatype, uint64_t ts)
{
    spb_metric_t *m = &n->metrics[i];
    memset(m, 0, sizeof(*m));
    m->name = name;
    m->alias = alias;
    m->datatype = datatype;
    m->timestamp = ts;
    return m;
}

/* Exact report-by-exception key of 64-bit integers and strings (MB-6). */
static uint64_t rbe_key(const gw_tag_cfg_t *t, const gw_tag_value_t *v)
{
    switch (gw_conv_kind(t)) {
    case GW_VK_INT64:
        return (uint64_t)v->i64;
    case GW_VK_UINT64:
        return v->u64;
    case GW_VK_BYTES:
        return gw_rbe_hash_bytes(v->str, 2u * t->length);
    default:
        return gw_rbe_hash(v->str);
    }
}

static void rbe_commit(gw_node_t *n, uint16_t tag, const gw_tag_value_t *v)
{
    const gw_tag_cfg_t *t = &n->cfg->tags[tag];
    if (gw_conv_kind(t) == GW_VK_DOUBLE) {
        gw_rbe_commit(&n->tag_rbe[tag], v->value, v->valid);
    } else {
        gw_rbe_commit_key(&n->tag_rbe[tag], v->valid ? rbe_key(t, v) : 0, v->valid);
    }
}

/* SPB-16 Quality of a value: GOOD when read, BAD when the read failed or the source is gone
 * (the value is then also null). */
static void set_quality(spb_metric_t *m, bool good)
{
    m->props.has_quality = true;
    m->props.quality = good ? SPB_QUALITY_GOOD : SPB_QUALITY_BAD;
}

/* SPB-18: the device is published as a template instance. */
static bool typed(const gw_node_t *n, uint16_t dev)
{
    return n->cfg->devices[dev].type[0] != '\0';
}

/* @p v must stay valid until the metric is published (strings are referenced, not copied).
 * Births carry the tag's properties (SPB-16: engUnit, engLow, engHigh, Quality); DATA messages
 * carry Quality only when it changes (a failed read, or the first good one after it). A
 * template member (@p member, SPB-18) always carries its name and no alias. */
static void tag_metric(gw_node_t *n, size_t i, uint16_t tag, const gw_tag_value_t *v, bool birth,
                       bool member)
{
    const gw_tag_cfg_t *t = &n->cfg->tags[tag];
    spb_metric_t *m = metric(n, i, birth || member ? t->name : NULL, GW_ALIAS_TAG_BASE + tag,
                             gw_conv_datatype(t), v->ts_ms);
    m->no_alias = member;
    m->is_null = !v->valid;
    if (birth) {
        m->props.eng_unit = t->units;
        m->props.has_eng_low = t->has_eng_low;
        m->props.eng_low = t->eng_low;
        m->props.has_eng_high = t->has_eng_high;
        m->props.eng_high = t->eng_high;
    }
    if (birth || !n->tag_rbe[tag].reported || n->tag_rbe[tag].valid != v->valid) {
        set_quality(m, v->valid);
    }
    gw_val_kind_t kind = gw_conv_kind(t);
    switch (kind) {
    case GW_VK_INT64:
        m->v.i64 = v->i64;
        break;
    case GW_VK_UINT64:
        m->v.u64 = v->u64;
        break;
    case GW_VK_STRING:
        m->v.str = v->str;
        break;
    case GW_VK_BYTES:
        m->v.bytes.data = (const uint8_t *)v->str;
        m->v.bytes.len = 2u * t->length;
        break;
    default:
        m->v.num = v->value;
        break;
    }
    rbe_commit(n, tag, v);
}

static bool tag_changed(const gw_node_t *n, uint16_t tag, const gw_tag_value_t *v)
{
    const gw_tag_cfg_t *t = &n->cfg->tags[tag];
    double deadband = t->type == GW_TYPE_BOOL ? 0.0 : t->deadband;
    gw_val_kind_t kind = gw_conv_kind(t);
    if (kind == GW_VK_DOUBLE) {
        return gw_rbe_changed(&n->tag_rbe[tag], v->value, v->valid, deadband);
    }
    return gw_rbe_changed_key(&n->tag_rbe[tag], v->valid ? rbe_key(t, v) : 0,
                              kind == GW_VK_INT64, v->valid, deadband);
}

/* ---------- births and deaths ---------- */

size_t gw_node_death_payload(uint64_t bdseq, uint8_t *out, size_t cap)
{
    spb_metric_t m = {
        .name = NAME_BDSEQ, .alias = GW_ALIAS_BDSEQ, .datatype = SPB_DT_UINT64,
        .v.u64 = bdseq};
    spb_payload_t p = {.timestamp = gw_port_time_ms(), .has_seq = false, .metrics = &m,
                       .metric_count = 1};
    return spb_encode(&p, out, cap);
}

static size_t template_definitions(gw_node_t *n, size_t i, uint64_t now);

/* ---------- SPB-17 diagnostics ---------- */

static void mb_cell(const void *ctx, uint32_t row, uint32_t col, spb_value_t *out)
{
    const gw_node_t *n = ctx;
    const gw_dev_stats_t *st = &n->diag_stats[row];
    const uint32_t counters[] = {st->cycles,      st->failed_cycles, st->bus.ok,
                                 st->bus.timeout, st->bus.crc,       st->bus.frame,
                                 st->bus.exception};
    if (col == 0) {
        out->str = n->cfg->devices[row].name;
    } else if (col == 1) {
            out->num = (n->diag_online >> row) & 1u;
    } else {
        out->num = counters[col - 2];
    }
}

static void tcp_cell(const void *ctx, uint32_t row, uint32_t col, spb_value_t *out)
{
    const gw_node_t *n = ctx;
    bool other = row >= n->cfg->device_count;
    const mb_stats_t *st = other ? &n->diag_tcp_other : &n->diag_stats[row].tcp;
    const uint32_t counters[] = {st->ok, st->timeout, st->crc, st->frame, st->exception};
    switch (col) {
    case 0:
        out->str = other ? OTHER_UNITS : n->cfg->devices[row].name;
        break;
    case 1:
        out->num = other ? 0 : n->cfg->devices[row].unit_id;
        break;
    case 2:
        out->num = other ? 0 : (n->diag_online >> row) & 1u;
        break;
    default:
        out->num = counters[col - 3];
        break;
    }
}

/* Snapshot the counters the DataSet publishes (the encoder reads each cell more than once:
 * they must not change meanwhile). Returns a hash of the snapshot. */
static uint64_t mb_snapshot(gw_node_t *n)
{
    n->diag_online = 0;
    for (uint16_t d = 0; d < n->cfg->device_count; d++) {
        gw_tagdb_get_stats(d, &n->diag_stats[d]);
        if (gw_tagdb_device_online(d)) {
            n->diag_online |= 1u << d;
        }
    }
    gw_mbap_stats_t g;
    gw_mbap_stats_get(&g);
    n->diag_tcp_other = g.other_units;
    uint64_t h = gw_rbe_hash_bytes(n->diag_stats, n->cfg->device_count * sizeof(n->diag_stats[0]));
    h ^= gw_rbe_hash_bytes(&n->diag_tcp_other, sizeof(n->diag_tcp_other)) * 31u;
    return h ^ ((uint64_t)n->diag_online << 32);
}

/* Append the diagnostics to n->metrics from @p i (the Modbus DataSet only if @p modbus and the
 * node has devices). Returns how many. */
static size_t diag_metrics(gw_node_t *n, size_t i, uint64_t now, bool birth, bool modbus)
{
    if (n->ops.diagnostics == NULL) {
        return 0;
    }
    gw_node_diag_t dg;
    memset(&dg, 0, sizeof(dg));
    n->ops.diagnostics(n->ops.ctx, &dg);
    size_t k = i;
    metric(n, k++, birth ? NAME_UPTIME : NULL, GW_ALIAS_UPTIME, SPB_DT_UINT64, now)->v.u64 =
        dg.uptime_s;
    metric(n, k++, birth ? NAME_FREE_HEAP : NULL, GW_ALIAS_FREE_HEAP, SPB_DT_UINT32, now)->v.num =
        dg.free_heap;
    metric(n, k++, birth ? NAME_MIN_HEAP : NULL, GW_ALIAS_MIN_HEAP, SPB_DT_UINT32, now)->v.num =
        dg.min_free_heap;
    if (birth) {
        n->has_rssi = dg.has_rssi;
    }
    if (n->has_rssi) {
        spb_metric_t *m = metric(n, k++, birth ? NAME_RSSI : NULL, GW_ALIAS_RSSI, SPB_DT_INT32, now);
        m->v.num = dg.rssi;
        m->is_null = !dg.rssi_valid;
    }
    if (modbus && n->cfg->device_count > 0) {
        n->mb_ds = (spb_dataset_t){.columns = 9, .names = k_mb_cols, .types = k_mb_types,
                                   .rows = n->cfg->device_count, .cell = mb_cell, .ctx = n};
        metric(n, k++, birth ? NAME_MODBUS : NULL, GW_ALIAS_MODBUS, SPB_DT_DATASET, now)->v.ds =
            &n->mb_ds;
    }
    if (modbus && n->cfg->modbus_tcp.enabled) {   /* #81 */
        n->mbtcp_ds = (spb_dataset_t){.columns = 8, .names = k_tcp_cols, .types = k_tcp_types,
                                      .rows = (uint32_t)n->cfg->device_count + 1u,
                                      .cell = tcp_cell, .ctx = n};
        metric(n, k++, birth ? NAME_MODBUS_TCP : NULL, GW_ALIAS_MODBUS_TCP, SPB_DT_DATASET, now)
            ->v.ds = &n->mbtcp_ds;
    }
    return k - i;
}

void gw_node_set_time_synced(gw_node_t *n, bool synced)
{
    if (!n->online || synced == n->time_synced_reported) {
        return;
    }
    n->time_synced_reported = synced;
    metric(n, 0, NULL, GW_ALIAS_TIME_SYNCED, SPB_DT_BOOLEAN, gw_port_time_ms())->v.num = synced;
    publish(n, "NDATA", NULL, 1, 0);
}

static void diag_schedule(gw_node_t *n, uint64_t now)
{
    uint32_t s = n->cfg->sparkplug.diagnostics_s;
    n->diag_due_ms = s == 0 ? 0 : now + (uint64_t)s * 1000u;
}

uint32_t gw_node_diag_tick(gw_node_t *n, uint64_t now_ms)
{
    if (n->diag_restart) {
        n->diag_restart = false;
        diag_schedule(n, now_ms);
    }
    if (!n->online || n->diag_due_ms == 0) {
        return UINT32_MAX;
    }
    if (now_ms < n->diag_due_ms) {
        uint64_t left = n->diag_due_ms - now_ms;
        return left < UINT32_MAX ? (uint32_t)left : UINT32_MAX - 1u;
    }
    diag_schedule(n, now_ms);
    uint64_t h = mb_snapshot(n);
    bool modbus = h != n->diag_hash;   /* rate limited: the counters only when they changed */
    n->diag_hash = h;
    size_t count = diag_metrics(n, 0, gw_port_time_ms(), false, modbus);
    if (count > 0) {
        publish(n, "NDATA", NULL, count, 0);
    }
    return n->cfg->sparkplug.diagnostics_s * 1000u;
}

static bool publish_nbirth(gw_node_t *n)
{
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    uint64_t now = gw_port_time_ms();
    size_t i = 0;
    metric(n, i++, NAME_BDSEQ, GW_ALIAS_BDSEQ, SPB_DT_UINT64, now)->v.u64 = n->bdseq;
    metric(n, i++, NAME_REBIRTH, GW_ALIAS_REBIRTH, SPB_DT_BOOLEAN, now)->v.num = 0;
    metric(n, i++, NAME_REBOOT, GW_ALIAS_REBOOT, SPB_DT_BOOLEAN, now)->v.num = 0;
    metric(n, i++, NAME_FIRMWARE, GW_ALIAS_FIRMWARE, SPB_DT_STRING, now)->v.str = n->info.firmware;
    metric(n, i++, NAME_HARDWARE, GW_ALIAS_HARDWARE, SPB_DT_STRING, now)->v.str = n->info.hardware;
    metric(n, i++, NAME_RESET_REASON, GW_ALIAS_RESET_REASON, SPB_DT_STRING, now)->v.str =
        n->info.reset_reason;
    metric(n, i++, NAME_SCAN_RATE, GW_ALIAS_SCAN_RATE, SPB_DT_UINT32, now)->v.num =
        n->scan_rate_ms;
    metric(n, i++, NAME_FW_UPDATE, GW_ALIAS_FW_UPDATE, SPB_DT_STRING, now)->v.str = "";
    metric(n, i++, NAME_FW_STATE, GW_ALIAS_FW_STATE, SPB_DT_STRING, now)->v.str = n->fw_state;
    metric(n, i++, NAME_FW_PROGRESS, GW_ALIAS_FW_PROGRESS, SPB_DT_UINT32, now)->v.num =
        n->fw_progress;
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        metric(n, i++, k_di_names[k], GW_ALIAS_DI_BASE + k, SPB_DT_BOOLEAN,
               io.di_ts_ms ? io.di_ts_ms : now)
            ->v.num = (io.di >> k) & 1u;
    }
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        spb_metric_t *m = metric(n, i++, k_do_names[k], GW_ALIAS_DO_BASE + k, SPB_DT_BOOLEAN,
                                 io.do_ts_ms ? io.do_ts_ms : now);
        m->v.num = (io.dout >> k) & 1u;
        m->is_null = io.do_unknown;   /* the latch may hold anything: never claim "off" */
        set_quality(m, !io.do_unknown);
    }
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        metric(n, i++, k_count_names[k], GW_ALIAS_COUNT_BASE + k, SPB_DT_UINT64,
               io.di_ts_ms ? io.di_ts_ms : now)
            ->v.u64 = io.count[k];
    }
    memcpy(n->count_reported, io.count, sizeof(n->count_reported));
    n->di_reported = io.di;
    n->do_reported = io.dout;
    n->do_unknown_reported = io.do_unknown;
    n->diag_hash = mb_snapshot(n);
    i += diag_metrics(n, i, now, true, true);
    if (n->ops.diagnostics != NULL) {   /* TIME-3 */
        gw_node_diag_t dg;
        memset(&dg, 0, sizeof(dg));
        n->ops.diagnostics(n->ops.ctx, &dg);
        n->time_synced_reported = dg.time_synced;
        metric(n, i++, NAME_TIME_SYNCED, GW_ALIAS_TIME_SYNCED, SPB_DT_BOOLEAN, now)->v.num =
            dg.time_synced;
    }
    n->diag_restart = true;   /* the period starts again at the next tick */
    i += template_definitions(n, i, now);
    return publish(n, "NBIRTH", NULL, i, 0);
}

/* Publish @p dev's metrics: n->metrics[0..count) for a plain device; for a typed one (SPB-18)
 * the members are n->metrics[1..count] and n->metrics[0] becomes their template instance. */
/* Returns 0, GW_NODE_PUBLISH_TOO_LARGE or -1 (publish_rc). */
static int publish_device_rc(gw_node_t *n, const char *type, uint16_t dev, size_t count,
                             bool birth, bool historical)
{
    const gw_device_cfg_t *d = &n->cfg->devices[dev];
    if (!typed(n, dev)) {
        return publish_rc(n, type, d->name, count, 0);
    }
    n->tmpl = (spb_template_t){.ref = d->type, .is_definition = false, .count = (uint32_t)count,
                               .members = &n->metrics[1]};
    metric(n, 0, birth ? d->type : NULL, GW_ALIAS_TAG_BASE + d->first_tag, SPB_DT_TEMPLATE,
           gw_port_time_ms())
        ->v.tmpl = &n->tmpl;
    n->metrics[0].is_historical = historical;   /* SPB-14: the instance of replayed members */
    return publish_rc(n, type, d->name, 1, 0);
}

static bool publish_device(gw_node_t *n, const char *type, uint16_t dev, size_t count, bool birth,
                           bool historical)
{
    return publish_device_rc(n, type, dev, count, birth, historical) == 0;
}

static void publish_dbirth(gw_node_t *n, uint16_t dev)
{
    const gw_device_cfg_t *d = &n->cfg->devices[dev];
    size_t base = typed(n, dev) ? 1 : 0;
    for (uint16_t i = 0; i < d->tag_count; i++) {
        tag_metric(n, base + i, (uint16_t)(d->first_tag + i), &n->vals[i], true, base != 0);
    }
    /* born only if the DBIRTH went out (issue #73: it may be larger than the broker accepts):
     * no DDATA without it. Issue #76: one that is too large is not rebuilt on every change of
     * the device; the next session or rebirth tries again */
    int r = publish_device_rc(n, "DBIRTH", dev, d->tag_count, true, false);
    n->dev_born[dev] = r == 0;
    if (r == GW_NODE_PUBLISH_TOO_LARGE) {
        n->dev_too_large[dev] = true;
        GW_LOGW(TAG, "DBIRTH %s too large for the broker: device not born, not retried until "
                     "the next session or rebirth",
                d->name);
    }
}

/* SPB-18 definition member @p i of a device type: name, datatype, a zero default value and
 * the engineering properties; no Quality (a definition holds no reading). */
static void def_member(const void *ctx, uint32_t i, spb_metric_t *out)
{
    const gw_node_def_ctx_t *c = ctx;
    const gw_tag_cfg_t *t = &c->cfg->tags[c->cfg->devices[c->dev].first_tag + i];
    memset(out, 0, sizeof(*out));
    out->name = t->name;
    out->no_alias = true;
    out->datatype = gw_conv_datatype(t);
    if (out->datatype == SPB_DT_STRING) {
        out->v.str = "";
    }
    out->props.eng_unit = t->units;
    out->props.has_eng_low = t->has_eng_low;
    out->props.eng_low = t->eng_low;
    out->props.has_eng_high = t->has_eng_high;
    out->props.eng_high = t->eng_high;
}

/* SPB-18: one definition per device type, after the node's own metrics. Returns the count. */
static size_t template_definitions(gw_node_t *n, size_t i, uint64_t now)
{
    size_t k = 0;
    for (uint16_t dev = 0; dev < n->cfg->device_count; dev++) {
        const gw_device_cfg_t *d = &n->cfg->devices[dev];
        bool first = d->type[0] != '\0';
        for (uint16_t j = 0; first && j < dev; j++) {
            first = strcmp(n->cfg->devices[j].type, d->type) != 0;
        }
        if (!first) {
            continue;
        }
        n->def_ctx[k].cfg = n->cfg;
        n->def_ctx[k].dev = dev;
        n->defs[k] = (spb_template_t){.ref = NULL, .is_definition = true, .count = d->tag_count,
                                      .get = def_member, .ctx = &n->def_ctx[k]};
        spb_metric_t *m = metric(n, i + k, d->type, 0, SPB_DT_TEMPLATE, now);
        m->no_alias = true;
        m->v.tmpl = &n->defs[k];
        k++;
    }
    return k;
}

static void publish_ddeath(gw_node_t *n, uint16_t dev)
{
    publish(n, "DDEATH", n->cfg->devices[dev].name, 0, 0);
    n->dev_born[dev] = false;
}

void gw_node_set_firmware_status(gw_node_t *n, const char *state, uint32_t progress)
{
    char s[GW_NODE_FW_STATE_MAX];
    if ((size_t)snprintf(s, sizeof(s), "%s", state) >= sizeof(s)) {
        GW_LOGW(TAG, "Firmware Update/State: %u characters, published cut to %u",
                (unsigned)strlen(state), (unsigned)(sizeof(s) - 1));   /* issue #87: not silent */
    }
    bool st = strcmp(s, n->fw_state) != 0;
    bool pr = progress != n->fw_progress;
    if (!st && !pr) {
        return;
    }
    memcpy(n->fw_state, s, sizeof(s));
    n->fw_progress = progress;
    if (!n->online) {
        return;   /* the next NBIRTH carries it */
    }
    uint64_t now = gw_port_time_ms();
    size_t i = 0;
    if (st) {
        metric(n, i++, NULL, GW_ALIAS_FW_STATE, SPB_DT_STRING, now)->v.str = n->fw_state;
    }
    if (pr) {
        metric(n, i++, NULL, GW_ALIAS_FW_PROGRESS, SPB_DT_UINT32, now)->v.num = n->fw_progress;
    }
    publish(n, "NDATA", NULL, i, 0);
}

bool gw_node_rebirth(gw_node_t *n)
{
    n->seq = 0;
    if (!publish_nbirth(n)) {
        return false;   /* no DBIRTH without the NBIRTH they belong to */
    }
    memset(n->dev_too_large, 0, sizeof(n->dev_too_large));   /* issue #76: try once more */
    for (uint16_t dev = 0; dev < n->cfg->device_count; dev++) {
        bool online;
        n->dev_born[dev] = false;
        gw_tagdb_get_device(n->cfg, dev, n->vals, &online);
        if (online) {
            publish_dbirth(n, dev);
        }
    }
    return true;
}

bool gw_node_session_up(gw_node_t *n, uint64_t bdseq)
{
    n->bdseq = bdseq;
    n->online = true;
    if (!gw_node_rebirth(n)) {
        gw_node_session_down(n);   /* issue #71: nothing went out: the node is not online */
        return false;
    }
    n->born_once = true;
    return true;
}

void gw_node_session_down(gw_node_t *n)
{
    n->online = false;
    memset(n->dev_born, 0, sizeof(n->dev_born));
}

/* ---------- report by exception ---------- */

/* SPB-14: what DDATA would have carried, into the store (same RBE and deadband rules). */
static void store_device(gw_node_t *n, uint16_t dev)
{
    bool online;
    gw_tagdb_get_device(n->cfg, dev, n->vals, &online);
    if (!online) {
        return;   /* a DDEATH is not stored: the DBIRTH of the next session tells the state */
    }
    const gw_device_cfg_t *d = &n->cfg->devices[dev];
    bool begun = false;
    for (uint16_t i = 0; i < d->tag_count; i++) {
        uint16_t tag = (uint16_t)(d->first_tag + i);
        const gw_tag_value_t *v = &n->vals[i];
        if (!tag_changed(n, tag, v)) {
            continue;
        }
        if (!begun) {
            gw_sf_begin(n->sf);
            begun = true;
        }
        /* MB-6: 64-bit integers exact (their bits), strings in continuation records */
        switch (gw_conv_kind(&n->cfg->tags[tag])) {
        case GW_VK_INT64:
            gw_sf_push_u64(n->sf, (uint8_t)dev, tag, (uint64_t)v->i64, !v->valid, v->ts_ms);
            break;
        case GW_VK_UINT64:
            gw_sf_push_u64(n->sf, (uint8_t)dev, tag, v->u64, !v->valid, v->ts_ms);
            break;
        case GW_VK_STRING:
            gw_sf_push_str(n->sf, (uint8_t)dev, tag, v->str, !v->valid, v->ts_ms);
            break;
        case GW_VK_BYTES:   /* SPB-19: raw bytes, NUL included */
            gw_sf_push_bytes(n->sf, (uint8_t)dev, tag, v->str, 2u * n->cfg->tags[tag].length,
                             !v->valid, v->ts_ms);
            break;
        default:
            gw_sf_push(n->sf, (uint8_t)dev, tag, v->value, !v->valid, v->ts_ms);
            break;
        }
        rbe_commit(n, tag, v);
    }
}

void gw_node_device_changed(gw_node_t *n, uint16_t dev)
{
    if (dev >= n->cfg->device_count) {
        return;
    }
    if (!n->online) {
        if (n->sf != NULL && n->born_once) {
            store_device(n, dev);
        }
        return;
    }
    bool online;
    gw_tagdb_get_device(n->cfg, dev, n->vals, &online);
    if (online && !n->dev_born[dev]) {
        if (!n->dev_too_large[dev]) {   /* issue #76 */
            publish_dbirth(n, dev);
        }
        return;
    }
    if (!online) {
        if (n->dev_born[dev]) {
            publish_ddeath(n, dev);
        }
        return;
    }
    const gw_device_cfg_t *d = &n->cfg->devices[dev];
    size_t base = typed(n, dev) ? 1 : 0;
    size_t count = 0;
    for (uint16_t i = 0; i < d->tag_count; i++) {
        uint16_t tag = (uint16_t)(d->first_tag + i);
        if (tag_changed(n, tag, &n->vals[i])) {
            tag_metric(n, base + count++, tag, &n->vals[i], false, base != 0);
        }
    }
    if (count > 0) {
        publish_device(n, "DDATA", dev, count, false, false);
    }
}

static void publish_io(gw_node_t *n, uint8_t force_do)
{
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    uint8_t di_changed = io.di ^ n->di_reported;
    uint8_t do_changed = (uint8_t)((io.dout ^ n->do_reported) | force_do);
    bool do_quality = io.do_unknown != n->do_unknown_reported;
    if (do_quality) {
        do_changed = 0xFF;   /* expander lost or recovered: every DO changes (null <-> value) */
    }
    size_t count = 0;
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        if (di_changed & (1u << k)) {
            metric(n, count++, NULL, GW_ALIAS_DI_BASE + k, SPB_DT_BOOLEAN, io.di_ts_ms)->v.num =
                (io.di >> k) & 1u;
        }
    }
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        if (do_changed & (1u << k)) {
            spb_metric_t *m = metric(n, count++, NULL, GW_ALIAS_DO_BASE + k, SPB_DT_BOOLEAN,
                                     io.do_ts_ms);
            m->v.num = (io.dout >> k) & 1u;
            m->is_null = io.do_unknown;
            if (do_quality) {
                set_quality(m, !io.do_unknown);
            }
        }
    }
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {   /* IO-4: with the DI change that made it */
        if (io.count[k] != n->count_reported[k]) {
            metric(n, count++, NULL, GW_ALIAS_COUNT_BASE + k, SPB_DT_UINT64, io.di_ts_ms)->v.u64 =
                io.count[k];
        }
    }
    memcpy(n->count_reported, io.count, sizeof(n->count_reported));
    n->di_reported = io.di;
    n->do_reported = io.dout;
    n->do_unknown_reported = io.do_unknown;
    if (count > 0) {
        publish(n, "NDATA", NULL, count, 0);
    }
}

/* SPB-14: what NDATA would have carried, into the store. */
static void store_io(gw_node_t *n)
{
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    uint8_t di_changed = io.di ^ n->di_reported;
    uint8_t do_changed = io.dout ^ n->do_reported;
    if (io.do_unknown != n->do_unknown_reported) {
        do_changed = 0xFF;
    }
    bool counts = memcmp(io.count, n->count_reported, sizeof(io.count)) != 0;
    if ((di_changed | do_changed) == 0 && !counts) {
        return;
    }
    gw_sf_begin(n->sf);
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        if (di_changed & (1u << k)) {
            gw_sf_push(n->sf, GW_SF_NODE, (uint16_t)(GW_ALIAS_DI_BASE + k), (io.di >> k) & 1u,
                       false, io.di_ts_ms);
        }
    }
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        if (do_changed & (1u << k)) {
            gw_sf_push(n->sf, GW_SF_NODE, (uint16_t)(GW_ALIAS_DO_BASE + k), (io.dout >> k) & 1u,
                       io.do_unknown, io.do_ts_ms);
        }
    }
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {   /* IO-4 pulse counters, UInt64 */
        if (io.count[k] != n->count_reported[k]) {
            gw_sf_push_u64(n->sf, GW_SF_NODE, (uint16_t)(GW_ALIAS_COUNT_BASE + k), io.count[k],
                           false, io.di_ts_ms);
        }
    }
    n->di_reported = io.di;
    n->do_reported = io.dout;
    n->do_unknown_reported = io.do_unknown;
    memcpy(n->count_reported, io.count, sizeof(n->count_reported));
}

void gw_node_io_changed(gw_node_t *n)
{
    if (n->online) {
        publish_io(n, 0);
    } else if (n->sf != NULL && n->born_once) {
        store_io(n);
    }
}

void gw_node_set_store(gw_node_t *n, gw_sf_t *sf)
{
    n->sf = sf;
}

size_t gw_node_flush_history(gw_node_t *n, size_t max_msgs)
{
    if (n->sf == NULL) {
        return 0;
    }
    for (size_t sent = 0; n->online && sent < max_msgs && gw_sf_count(n->sf) > 0;) {
        size_t cnt = gw_sf_peek(n->sf, n->hist, GW_NODE_HIST_RECS);
        if (cnt == 0) {
            break;   /* the spill cannot be read: the rest stays */
        }
        uint8_t dev = n->hist[0].dev;
        bool node = dev == GW_SF_NODE;
        if (!node && (dev >= n->cfg->device_count || !n->dev_born[dev])) {
            /* no DBIRTH in this session (offline now): DDATA would break the protocol */
            n->sf_discarded += (uint32_t)cnt;
            gw_sf_pop(n->sf, cnt);
            continue;
        }
        size_t nm = 0;
        size_t base = !node && typed(n, dev) ? 1 : 0;   /* SPB-18: members of the instance */
        for (size_t i = 0; i < cnt && nm < GW_MAX_TAGS_PER_DEVICE;) {
            const gw_sf_rec_t *r = &n->hist[i];
            size_t used = gw_sf_sample(r, cnt - i, n->hist_str[nm], sizeof(n->hist_str[nm]));
            i += used;
            if (!node && r->metric >= n->cfg->tag_count) {
                n->sf_discarded++;
                continue;
            }
            const gw_tag_cfg_t *t = node ? NULL : &n->cfg->tags[r->metric];
            bool counter = node && r->metric >= GW_ALIAS_COUNT_BASE &&
                           r->metric < GW_ALIAS_COUNT_BASE + GW_NODE_IO_COUNT;
            uint32_t dt = counter ? SPB_DT_UINT64 : node ? SPB_DT_BOOLEAN : gw_conv_datatype(t);
            spb_metric_t *m = metric(n, base + nm, base ? t->name : NULL,
                                     node ? r->metric : GW_ALIAS_TAG_BASE + r->metric, dt, r->ts_ms);
            m->no_alias = base != 0;
            switch (counter ? GW_VK_UINT64 : node ? GW_VK_DOUBLE : gw_conv_kind(t)) {
            case GW_VK_INT64:
                m->v.i64 = r->v.i64;
                break;
            case GW_VK_UINT64:
                m->v.u64 = r->v.u64;
                break;
            case GW_VK_STRING:
                m->v.str = n->hist_str[nm];
                break;
            case GW_VK_BYTES:
                m->v.bytes.data = (const uint8_t *)n->hist_str[nm];
                m->v.bytes.len = 2u * t->length;
                break;
            default:
                m->v.num = r->v.num;
                break;
            }
            m->is_null = (r->flags & GW_SF_NULL) != 0;
            m->is_historical = true;
            /* SPB-16: a replayed value always says its quality (the live ones only on a change,
             * which the replay cannot tell apart): tags and DOs */
            bool is_do = node && r->metric >= GW_ALIAS_DO_BASE &&
                         r->metric < GW_ALIAS_DO_BASE + GW_NODE_IO_COUNT;
            if (!node || is_do) {
                set_quality(m, !m->is_null);
            }
            nm++;
        }
        bool sent_ok = nm == 0 ||
                       (node ? publish(n, "NDATA", NULL, nm, 0)
                             : publish_device(n, "DDATA", dev, nm, false, true));
        if (!sent_ok) {
            break;   /* kept: retried on the next call (or the next session) */
        }
        gw_sf_pop(n->sf, cnt);
        n->sf_sent++;
        sent++;
    }
    return gw_sf_count(n->sf);
}

void gw_node_write_done(gw_node_t *n, uint16_t tag, bool ok)
{
    if (!ok || !n->online || tag >= n->cfg->tag_count) {
        return;
    }
    uint16_t dev = gw_config_tag_device(n->cfg, tag);
    if (dev == GW_NO_DEVICE || !n->dev_born[dev]) {
        return;
    }
    gw_tag_value_t v;
    gw_tagdb_get_tag(tag, &v);
    size_t base = typed(n, dev) ? 1 : 0;
    tag_metric(n, base, tag, &v, false, base != 0);
    publish_device(n, "DDATA", dev, 1, false, false);
}

/* ---------- commands ---------- */

static bool rx_true(const spb_rx_metric_t *m)
{
    double v;
    return spb_rx_number(m, SPB_DT_BOOLEAN, &v) && v != 0.0;
}

static void log_discard(const char *kind, const spb_rx_metric_t *m, const char *why)
{
    /* Show only what the command carried: a missing alias is not "alias 0". */
    if (m->has_alias) {
        GW_LOGW(TAG, "%s: metric '%s' (alias %" PRIu64 ") discarded: %s", kind,
                m->name ? m->name : "", m->alias, why);
    } else {
        GW_LOGW(TAG, "%s: metric '%s' discarded: %s", kind, m->name ? m->name : "", why);
    }
}

static bool on_ncmd_metric(void *ctx, const spb_rx_metric_t *m)
{
    gw_node_t *n = ctx;
    bool by_name = m->name != NULL;
    if (by_name ? strcmp(m->name, NAME_REBIRTH) == 0 : m->alias == GW_ALIAS_REBIRTH) {
        n->cmd_rebirth |= rx_true(m);
        return true;
    }
    if (by_name ? strcmp(m->name, NAME_SCAN_RATE) == 0 : m->alias == GW_ALIAS_SCAN_RATE) {
        double v;
        n->cmd_scan_rate = true;   /* NDATA confirms the rate in force, applied or not */
        if (!spb_rx_number(m, m->has_datatype ? m->datatype : SPB_DT_UINT32, &v) ||
            v != floor(v) || (v != 0.0 && (v < GW_SCAN_RATE_MIN_MS || v > GW_SCAN_RATE_MAX_MS))) {
            log_discard("NCMD", m, "Scan Rate must be 0 (per-device poll_ms) or 50..3600000 ms");
            return true;
        }
        n->scan_rate_ms = (uint32_t)v;
        n->ops.set_scan_rate(n->ops.ctx, n->scan_rate_ms);
        GW_LOGI(TAG, "NCMD Scan Rate %" PRIu32 " ms%s", n->scan_rate_ms,
                n->scan_rate_ms == 0 ? " (each device at its own poll_ms)" : "");
        return true;
    }
    if (by_name ? strcmp(m->name, NAME_REBOOT) == 0 : m->alias == GW_ALIAS_REBOOT) {
        n->cmd_reboot |= rx_true(m);
        return true;
    }
    if (by_name ? strcmp(m->name, NAME_FW_UPDATE) == 0 : m->alias == GW_ALIAS_FW_UPDATE) {
        /* FW-1: the payload decoded in full before this pass: the URL is complete */
        if (m->is_null || m->kind != SPB_VAL_STRING || m->v.s[0] == '\0') {
            log_discard("NCMD", m, "Firmware Update needs the image URL as a string");
        } else if (n->ops.firmware_update == NULL) {
            log_discard("NCMD", m, "firmware updates not supported");
        } else {
            GW_LOGW(TAG, "NCMD Firmware Update from %s", m->v.s);
            n->ops.firmware_update(n->ops.ctx, m->v.s);
        }
        return true;
    }
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        if (by_name ? strcmp(m->name, k_do_names[k]) == 0 : m->alias == GW_ALIAS_DO_BASE + k) {
            double v;
            if (!spb_rx_number(m, SPB_DT_BOOLEAN, &v)) {
                log_discard("NCMD", m, "no boolean value");
                return true;
            }
            if (!n->ops.set_do(n->ops.ctx, k, v != 0.0)) {
                GW_LOGE(TAG, "NCMD: DO/%u could not be driven", k + 1);
            }
            n->cmd_do_mask |= (uint8_t)(1u << k);  /* confirm the actual state either way */
            return true;
        }
    }
    if (!by_name && !m->has_alias) {
        log_discard("NCMD", m, "neither name nor alias");
    } else {
        log_discard("NCMD", m, "unknown or read-only metric");
    }
    return true;
}

static int resolve_tag(const gw_node_t *n, uint16_t dev, const spb_rx_metric_t *m)
{
    const gw_device_cfg_t *d = &n->cfg->devices[dev];
    if (m->name != NULL) {
        return gw_config_find_tag(n->cfg, dev, m->name);
    }
    if (typed(n, dev)) {
        return -1;   /* SPB-18: members have no alias; this one would be the instance's */
    }
    if (m->has_alias && m->alias >= GW_ALIAS_TAG_BASE + d->first_tag &&
        m->alias < GW_ALIAS_TAG_BASE + d->first_tag + d->tag_count) {
        return (int)(m->alias - GW_ALIAS_TAG_BASE);
    }
    return -1;
}

/* 2^63 as a double: the first value out of the int64 range. */
#define TWO63 9223372036854775808.0

/* Integral value of an incoming metric for a 64-bit tag, exactly (a double is not above 2^53):
 * *neg and *mag give the sign and the magnitude. */
static bool rx_integer(const spb_rx_metric_t *m, uint32_t dt, bool *neg, uint64_t *mag)
{
    if (m->is_null) {
        return false;
    }
    switch (m->kind) {
    case SPB_VAL_LONG:
        *neg = dt == SPB_DT_INT64 && (int64_t)m->v.l < 0;
        *mag = *neg ? 0u - m->v.l : m->v.l;
        return true;
    case SPB_VAL_INT:
    case SPB_VAL_BOOL:
    case SPB_VAL_FLOAT:
    case SPB_VAL_DOUBLE: {
        double d;
        if (!spb_rx_number(m, dt, &d) || d != floor(d) || d <= -TWO63 * 2.0 || d >= TWO63 * 2.0) {
            return false;
        }
        *neg = d < 0.0;
        *mag = (uint64_t)fabs(d);
        return true;
    }
    default:
        return false;
    }
}

/* Value of a DCMD metric for tag @p t, in the member the tag uses; @p why on failure. */
static bool rx_value(const gw_tag_cfg_t *t, const spb_rx_metric_t *m, gw_tag_value_t *out,
                     const char **why)
{
    uint32_t dt = m->has_datatype ? m->datatype : gw_conv_datatype(t);
    memset(out, 0, sizeof(*out));
    bool neg;
    uint64_t mag;
    switch (gw_conv_kind(t)) {
    case GW_VK_STRING:
        if (m->is_null || m->kind != SPB_VAL_STRING) {
            *why = "no string value";
            return false;
        }
        if (strlen(m->v.s) > 2u * t->length) {
            *why = "string longer than the tag";
            return false;
        }
        strcpy(out->str, m->v.s);
        return true;
    case GW_VK_BYTES:   /* SPB-19: exactly the tag's registers, nothing to pad */
        if (m->is_null || m->kind != SPB_VAL_BYTES) {
            *why = "no Bytes value";
            return false;
        }
        if (m->v.bytes.len != 2u * t->length) {
            *why = "Bytes value of another length than the tag";
            return false;
        }
        memcpy(out->str, m->v.bytes.data, m->v.bytes.len);
        return true;
    case GW_VK_INT64:
        if (!rx_integer(m, dt, &neg, &mag)) {
            *why = "no integer value";
            return false;
        }
        if (neg ? mag > (uint64_t)INT64_MAX + 1u : mag > (uint64_t)INT64_MAX) {
            *why = "value out of the int64 range";
            return false;
        }
        out->i64 = neg ? (int64_t)(0u - mag) : (int64_t)mag;
        return true;
    case GW_VK_UINT64:
        if (!rx_integer(m, dt, &neg, &mag)) {
            *why = "no integer value";
            return false;
        }
        if (neg && mag != 0) {
            *why = "negative value for a uint64 tag";
            return false;
        }
        out->u64 = mag;
        return true;
    default:
        if (!spb_rx_number(m, dt, &out->value)) {
            *why = "no numeric value";
            return false;
        }
        return true;
    }
}

/* Resolve one DCMD metric to a writable tag and its value; logs the reason of a discard
 * unless @p quiet (the counting pass must not log everything twice). */
static bool dcmd_resolve(const gw_node_t *n, const spb_rx_metric_t *m, bool quiet, int *tag_out,
                         gw_tag_value_t *value)
{
    int tag = resolve_tag(n, (uint16_t)n->cmd_dev, m);
    if (tag < 0) {
        if (!quiet) {
            log_discard("DCMD", m, "unknown metric");
        }
        return false;
    }
    const gw_tag_cfg_t *t = &n->cfg->tags[tag];
    if (!t->writable) {
        if (!quiet) {
            log_discard("DCMD", m, "read-only metric");
        }
        return false;
    }
    const char *why = "";
    if (!rx_value(t, m, value, &why)) {
        if (!quiet) {
            log_discard("DCMD", m, why);
        }
        return false;
    }
    *tag_out = tag;
    return true;
}

static bool on_dcmd_metric(void *ctx, const spb_rx_metric_t *m)
{
    gw_node_t *n = ctx;
    if (m->kind == SPB_VAL_TEMPLATE) {
        return true;   /* SPB-18: the instance; its members came through on_dcmd_metric */
    }
    int tag;
    gw_tag_value_t v;
    if (!dcmd_resolve(n, m, n->cmd_count_only, &tag, &v)) {
        return true;
    }
    if (n->cmd_count_only) {
        n->cmd_writes++;
        return true;
    }
    if (!n->ops.request_write(n->ops.ctx, (uint16_t)tag, &v)) {
        log_discard("DCMD", m, "write queue full");   /* not expected: room was checked */
    }
    return true;
}

/* Match "<prefix>/<rest>" where prefix is a literal; returns the rest or NULL. */
static const char *eat(const char *s, const char *end, const char *lit)
{
    size_t l = strlen(lit);
    if ((size_t)(end - s) < l || memcmp(s, lit, l) != 0) {
        return NULL;
    }
    return s + l;
}

void gw_node_handle_message(gw_node_t *n, const char *topic, size_t topic_len,
                            const uint8_t *payload, size_t len)
{
    const char *end = topic + topic_len;
    const char *p = eat(topic, end, "spBv1.0/");
    p = p ? eat(p, end, n->cfg->sparkplug.group_id) : NULL;
    p = p ? eat(p, end, "/") : NULL;
    if (p == NULL || !n->online) {
        return;
    }
    const char *q;
    if ((q = eat(p, end, "NCMD/")) != NULL) {
        q = eat(q, end, n->cfg->sparkplug.edge_node_id);
        if (q == NULL || q != end) {
            return;
        }
        n->cmd_rebirth = n->cmd_reboot = false;
        n->cmd_do_mask = 0;
        n->cmd_scan_rate = false;
        /* All or nothing: decode the whole payload first, then apply it. */
        if (!spb_decode(payload, len, NULL, NULL, NULL)) {
            GW_LOGW(TAG, "NCMD: undecodable payload (%u bytes: %s; strings up to %d characters, names up "
                         "to %d): nothing applied",
                    (unsigned)len, spb_last_error(), SPB_STRING_MAX, SPB_NAME_MAX);
            return;
        }
        spb_decode(payload, len, NULL, on_ncmd_metric, n);
        if (n->cmd_do_mask) {
            publish_io(n, n->cmd_do_mask);
        }
        if (n->cmd_scan_rate) {
            metric(n, 0, NULL, GW_ALIAS_SCAN_RATE, SPB_DT_UINT32, gw_port_time_ms())->v.num =
                n->scan_rate_ms;
            publish(n, "NDATA", NULL, 1, 0);
        }
        if (n->cmd_rebirth) {
            GW_LOGI(TAG, "NCMD rebirth");
            gw_node_rebirth(n);
        }
        if (n->cmd_reboot) {
            GW_LOGW(TAG, "NCMD reboot: publishing NDEATH and restarting");
            size_t dl = gw_node_death_payload(n->bdseq, n->buf, n->buf_cap);
            if (gw_node_topic(n, "NDEATH", NULL, n->topic, sizeof(n->topic))) {
                n->ops.publish(n->ops.ctx, n->topic, n->buf, dl, 1, false);
            }
            gw_node_session_down(n);
            n->ops.reboot(n->ops.ctx);
        }
        return;
    }
    if ((q = eat(p, end, "DCMD/")) != NULL) {
        q = eat(q, end, n->cfg->sparkplug.edge_node_id);
        q = q ? eat(q, end, "/") : NULL;
        if (q == NULL) {
            return;
        }
        int dev = gw_config_find_device(n->cfg, q, (size_t)(end - q));
        if (dev < 0) {
            GW_LOGW(TAG, "DCMD for unknown device '%.*s'", (int)(end - q), q);
            return;
        }
        if (!n->dev_born[dev]) {
            GW_LOGW(TAG, "DCMD for offline device '%s' discarded", n->cfg->devices[dev].name);
            return;
        }
        n->cmd_dev = dev;
        /* All or nothing: the whole payload must decode, and every write it carries must fit in
         * the bus queue; otherwise nothing is queued (issue #60). The counting pass decodes the
         * payload in full, so it also validates it. */
        n->cmd_count_only = true;
        n->cmd_writes = 0;
        /* SPB-18: the members of a template instance are writes like top-level metrics */
        const spb_decoder_t dec = {.metric = on_dcmd_metric, .member = on_dcmd_metric, .ctx = n};
        bool decoded = spb_decode_with(payload, len, NULL, &dec);
        n->cmd_count_only = false;
        if (!decoded) {
            GW_LOGW(TAG, "DCMD: undecodable payload (%u bytes: %s; strings up to %d characters, names up "
                         "to %d): nothing applied",
                    (unsigned)len, spb_last_error(), SPB_STRING_MAX, SPB_NAME_MAX);
            return;
        }
        size_t room = n->ops.write_room(n->ops.ctx);
        if (n->cmd_writes > room) {
            GW_LOGW(TAG, "DCMD for '%s': %u writes but the bus queue has room for %u: nothing "
                         "applied", n->cfg->devices[dev].name, (unsigned)n->cmd_writes,
                    (unsigned)room);
            return;
        }
        spb_decode_with(payload, len, NULL, &dec);
    }
}
