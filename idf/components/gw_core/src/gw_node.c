#include "gw_node.h"

#include <inttypes.h>
#include <string.h>

#include "gw_conv.h"
#include "gw_port.h"

static const char *TAG = "spb";

static const char *const k_di_names[GW_NODE_IO_COUNT] = {"DI/1", "DI/2", "DI/3", "DI/4",
                                                         "DI/5", "DI/6", "DI/7", "DI/8"};
static const char *const k_do_names[GW_NODE_IO_COUNT] = {"DO/1", "DO/2", "DO/3", "DO/4",
                                                         "DO/5", "DO/6", "DO/7", "DO/8"};
#define NAME_BDSEQ    "bdSeq"
#define NAME_REBIRTH  "Node Control/Rebirth"
#define NAME_REBOOT   "Node Control/Reboot"
#define NAME_FIRMWARE "Node Info/Firmware"
#define NAME_HARDWARE "Node Info/Hardware"
#define NAME_RESET_REASON "Node Info/Reset Reason"

/* ---------- helpers ---------- */

void gw_node_init(gw_node_t *n, const gw_config_t *cfg, const gw_node_ops_t *ops,
                  const gw_node_info_t *info)
{
    memset(n, 0, sizeof(*n));
    n->cfg = cfg;
    n->ops = *ops;
    n->info = *info;
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

static void publish(gw_node_t *n, const char *type, const char *device, size_t metric_count,
                    int qos)
{
    spb_payload_t p = {
        .timestamp = gw_port_time_ms(),
        .has_seq = true,
        .seq = next_seq(n),
        .metrics = n->metrics,
        .metric_count = metric_count,
    };
    size_t len = spb_encode(&p, n->buf, sizeof(n->buf));
    if (len == 0 || !gw_node_topic(n, type, device, n->topic, sizeof(n->topic))) {
        GW_LOGE(TAG, "%s%s%s: payload does not fit", type, device ? " " : "", device ? device : "");
        return;
    }
    if (n->ops.publish(n->ops.ctx, n->topic, n->buf, len, qos, false) < 0) {
        GW_LOGW(TAG, "publish %s failed", n->topic);
        return;
    }
    GW_LOGD(TAG, "%s seq=%" PRIu64 " metrics=%u bytes=%u", n->topic, p.seq, (unsigned)metric_count,
            (unsigned)len);
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

static void tag_metric(gw_node_t *n, size_t i, uint16_t tag, const gw_tag_value_t *v, bool birth)
{
    const gw_tag_cfg_t *t = &n->cfg->tags[tag];
    spb_metric_t *m = metric(n, i, birth ? t->name : NULL, GW_ALIAS_TAG_BASE + tag,
                             gw_conv_datatype(t), v->ts_ms);
    m->is_null = !v->valid;
    m->v.num = v->value;
    gw_rbe_commit(&n->tag_rbe[tag], v->value, v->valid);
}

/* ---------- births and deaths ---------- */

size_t gw_node_death_payload(uint64_t bdseq, uint8_t *out, size_t cap)
{
    spb_metric_t m = {
        .name = NAME_BDSEQ, .alias = GW_ALIAS_BDSEQ, .datatype = SPB_DT_UINT64,
        .v.num = (double)bdseq};
    spb_payload_t p = {.timestamp = gw_port_time_ms(), .has_seq = false, .metrics = &m,
                       .metric_count = 1};
    return spb_encode(&p, out, cap);
}

static void publish_nbirth(gw_node_t *n)
{
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    uint64_t now = gw_port_time_ms();
    size_t i = 0;
    metric(n, i++, NAME_BDSEQ, GW_ALIAS_BDSEQ, SPB_DT_UINT64, now)->v.num = (double)n->bdseq;
    metric(n, i++, NAME_REBIRTH, GW_ALIAS_REBIRTH, SPB_DT_BOOLEAN, now)->v.num = 0;
    metric(n, i++, NAME_REBOOT, GW_ALIAS_REBOOT, SPB_DT_BOOLEAN, now)->v.num = 0;
    metric(n, i++, NAME_FIRMWARE, GW_ALIAS_FIRMWARE, SPB_DT_STRING, now)->v.str = n->info.firmware;
    metric(n, i++, NAME_HARDWARE, GW_ALIAS_HARDWARE, SPB_DT_STRING, now)->v.str = n->info.hardware;
    metric(n, i++, NAME_RESET_REASON, GW_ALIAS_RESET_REASON, SPB_DT_STRING, now)->v.str =
        n->info.reset_reason;
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        metric(n, i++, k_di_names[k], GW_ALIAS_DI_BASE + k, SPB_DT_BOOLEAN,
               io.di_ts_ms ? io.di_ts_ms : now)
            ->v.num = (io.di >> k) & 1u;
    }
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        metric(n, i++, k_do_names[k], GW_ALIAS_DO_BASE + k, SPB_DT_BOOLEAN,
               io.do_ts_ms ? io.do_ts_ms : now)
            ->v.num = (io.dout >> k) & 1u;
    }
    n->di_reported = io.di;
    n->do_reported = io.dout;
    publish(n, "NBIRTH", NULL, i, 0);
}

static void publish_dbirth(gw_node_t *n, uint16_t dev)
{
    const gw_device_cfg_t *d = &n->cfg->devices[dev];
    for (uint16_t i = 0; i < d->tag_count; i++) {
        tag_metric(n, i, (uint16_t)(d->first_tag + i), &n->vals[i], true);
    }
    publish(n, "DBIRTH", d->name, d->tag_count, 0);
    n->dev_born[dev] = true;
}

static void publish_ddeath(gw_node_t *n, uint16_t dev)
{
    publish(n, "DDEATH", n->cfg->devices[dev].name, 0, 0);
    n->dev_born[dev] = false;
}

void gw_node_rebirth(gw_node_t *n)
{
    n->seq = 0;
    publish_nbirth(n);
    for (uint16_t dev = 0; dev < n->cfg->device_count; dev++) {
        bool online;
        n->dev_born[dev] = false;
        gw_tagdb_get_device(n->cfg, dev, n->vals, &online);
        if (online) {
            publish_dbirth(n, dev);
        }
    }
}

void gw_node_session_up(gw_node_t *n, uint64_t bdseq)
{
    n->bdseq = bdseq;
    n->online = true;
    gw_node_rebirth(n);
}

void gw_node_session_down(gw_node_t *n)
{
    n->online = false;
    memset(n->dev_born, 0, sizeof(n->dev_born));
}

/* ---------- report by exception ---------- */

void gw_node_device_changed(gw_node_t *n, uint16_t dev)
{
    if (!n->online || dev >= n->cfg->device_count) {
        return;
    }
    bool online;
    gw_tagdb_get_device(n->cfg, dev, n->vals, &online);
    if (online && !n->dev_born[dev]) {
        publish_dbirth(n, dev);
        return;
    }
    if (!online) {
        if (n->dev_born[dev]) {
            publish_ddeath(n, dev);
        }
        return;
    }
    const gw_device_cfg_t *d = &n->cfg->devices[dev];
    size_t count = 0;
    for (uint16_t i = 0; i < d->tag_count; i++) {
        uint16_t tag = (uint16_t)(d->first_tag + i);
        const gw_tag_cfg_t *t = &n->cfg->tags[tag];
        double deadband = t->type == GW_TYPE_BOOL ? 0.0 : t->deadband;
        if (gw_rbe_changed(&n->tag_rbe[tag], n->vals[i].value, n->vals[i].valid, deadband)) {
            tag_metric(n, count++, tag, &n->vals[i], false);
        }
    }
    if (count > 0) {
        publish(n, "DDATA", d->name, count, 0);
    }
}

static void publish_io(gw_node_t *n, uint8_t force_do)
{
    gw_local_io_t io;
    gw_tagdb_get_io(&io);
    uint8_t di_changed = io.di ^ n->di_reported;
    uint8_t do_changed = (uint8_t)((io.dout ^ n->do_reported) | force_do);
    size_t count = 0;
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        if (di_changed & (1u << k)) {
            metric(n, count++, NULL, GW_ALIAS_DI_BASE + k, SPB_DT_BOOLEAN, io.di_ts_ms)->v.num =
                (io.di >> k) & 1u;
        }
    }
    for (uint8_t k = 0; k < GW_NODE_IO_COUNT; k++) {
        if (do_changed & (1u << k)) {
            metric(n, count++, NULL, GW_ALIAS_DO_BASE + k, SPB_DT_BOOLEAN, io.do_ts_ms)->v.num =
                (io.dout >> k) & 1u;
        }
    }
    n->di_reported = io.di;
    n->do_reported = io.dout;
    if (count > 0) {
        publish(n, "NDATA", NULL, count, 0);
    }
}

void gw_node_io_changed(gw_node_t *n)
{
    if (n->online) {
        publish_io(n, 0);
    }
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
    tag_metric(n, 0, tag, &v, false);
    publish(n, "DDATA", n->cfg->devices[dev].name, 1, 0);
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
    if (by_name ? strcmp(m->name, NAME_REBOOT) == 0 : m->alias == GW_ALIAS_REBOOT) {
        n->cmd_reboot |= rx_true(m);
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
    if (m->has_alias && m->alias >= GW_ALIAS_TAG_BASE + d->first_tag &&
        m->alias < GW_ALIAS_TAG_BASE + d->first_tag + d->tag_count) {
        return (int)(m->alias - GW_ALIAS_TAG_BASE);
    }
    return -1;
}

static bool on_dcmd_metric(void *ctx, const spb_rx_metric_t *m)
{
    gw_node_t *n = ctx;
    int tag = resolve_tag(n, (uint16_t)n->cmd_dev, m);
    if (tag < 0) {
        log_discard("DCMD", m, "unknown metric");
        return true;
    }
    const gw_tag_cfg_t *t = &n->cfg->tags[tag];
    if (!t->writable) {
        log_discard("DCMD", m, "read-only metric");
        return true;
    }
    double v;
    if (!spb_rx_number(m, m->has_datatype ? m->datatype : gw_conv_datatype(t), &v)) {
        log_discard("DCMD", m, "no numeric value");
        return true;
    }
    if (!n->ops.request_write(n->ops.ctx, (uint16_t)tag, v)) {
        log_discard("DCMD", m, "write queue full");
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
        /* All or nothing: decode the whole payload first, then apply it. */
        if (!spb_decode(payload, len, NULL, NULL, NULL)) {
            GW_LOGW(TAG, "NCMD: undecodable payload (%u bytes): nothing applied", (unsigned)len);
            return;
        }
        spb_decode(payload, len, NULL, on_ncmd_metric, n);
        if (n->cmd_do_mask) {
            publish_io(n, n->cmd_do_mask);
        }
        if (n->cmd_rebirth) {
            GW_LOGI(TAG, "NCMD rebirth");
            gw_node_rebirth(n);
        }
        if (n->cmd_reboot) {
            GW_LOGW(TAG, "NCMD reboot: publishing NDEATH and restarting");
            size_t dl = gw_node_death_payload(n->bdseq, n->buf, sizeof(n->buf));
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
        if (!spb_decode(payload, len, NULL, NULL, NULL)) {
            GW_LOGW(TAG, "DCMD: undecodable payload (%u bytes): nothing applied", (unsigned)len);
            return;
        }
        spb_decode(payload, len, NULL, on_dcmd_metric, n);
    }
}
