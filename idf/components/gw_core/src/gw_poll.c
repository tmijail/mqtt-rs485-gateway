#include "gw_poll.h"

#include <math.h>
#include <string.h>

#include "gw_conv.h"
#include "gw_port.h"

static const char *TAG = "poll";

static mb_fc_t read_fc(gw_area_t area)
{
    switch (area) {
    case GW_AREA_COIL:
        return MB_FC_READ_COILS;
    case GW_AREA_DISCRETE:
        return MB_FC_READ_DISCRETE_INPUTS;
    case GW_AREA_HOLDING:
        return MB_FC_READ_HOLDING_REGISTERS;
    default:
        return MB_FC_READ_INPUT_REGISTERS;
    }
}

/* Value of tag @p t from a read response that starts at address @p start. */
static void decode_member(const gw_tag_cfg_t *t, const mb_response_t *rsp, uint16_t start,
                          gw_tag_value_t *out)
{
    uint16_t off = (uint16_t)(t->address - start);
    if (t->area == GW_AREA_COIL || t->area == GW_AREA_DISCRETE) {
        out->value = mb_rsp_bit(rsp, off) ? 1.0 : 0.0;
        return;
    }
    if (t->bit != GW_NO_BIT) {   /* MB-6: one bit of a holding/input register */
        out->value = (mb_rsp_reg(rsp, off) >> t->bit) & 1u ? 1.0 : 0.0;
        return;
    }
    uint16_t regs[GW_TAG_MAX_REGS];
    uint16_t width = gw_tag_width(t);
    for (uint16_t i = 0; i < width; i++) {
        regs[i] = mb_rsp_reg(rsp, (uint16_t)(off + i));
    }
    gw_conv_decode(t, regs, out);
}

/* One transaction to @p dev; the attempts it took are added to the device's counters. */
static mb_status_t execute_n(gw_poller_t *p, uint16_t dev, const mb_request_t *rq,
                             mb_response_t *rsp, uint8_t retries)
{
    const mb_stats_t before = p->mb->stats;
    mb_status_t st = mb_master_execute_retries(p->mb, rq, rsp, retries);
    const mb_stats_t *after = &p->mb->stats;
    mb_stats_t *acc = &p->stats[dev].bus;
    acc->ok += after->ok - before.ok;
    acc->timeout += after->timeout - before.timeout;
    acc->crc += after->crc - before.crc;
    acc->frame += after->frame - before.frame;
    acc->exception += after->exception - before.exception;
    return st;
}

static mb_status_t execute(gw_poller_t *p, uint16_t dev, const mb_request_t *rq, mb_response_t *rsp)
{
    return execute_n(p, dev, rq, rsp, p->mb->cfg.retries);
}

static bool is_offline(const gw_poller_t *p, uint16_t dev)
{
    return gw_devstate_probing(&p->state[dev], p->cfg->devices[dev].offline_after);
}

void gw_poller_init(gw_poller_t *p, const gw_config_t *cfg, mb_master_t *mb,
                    const gw_poller_opts_t *opts, uint32_t now_ms)
{
    memset(p, 0, sizeof(*p));
    p->cfg = cfg;
    p->mb = mb;
    p->opts = *opts;
    gw_plan_build(&p->plan, cfg, opts->merge_reads, opts->max_gap);
    for (uint16_t d = 0; d < cfg->device_count; d++) {
        p->next_due_ms[d] = now_ms;
    }
}

int gw_poller_next(const gw_poller_t *p, uint32_t now_ms, uint32_t *wait_ms)
{
    int best = -1;
    int32_t best_wait = INT32_MAX;
    for (uint16_t d = 0; d < p->cfg->device_count; d++) {
        int32_t w = (int32_t)(p->next_due_ms[d] - now_ms);
        if (w < best_wait) {
            best_wait = w;
            best = d;
        }
    }
    if (best < 0) {
        *wait_ms = UINT32_MAX;
        return -1;
    }
    if (best_wait > 0) {
        *wait_ms = (uint32_t)best_wait;
        return -1;
    }
    *wait_ms = 0;
    return best;
}

gw_dev_event_t gw_poller_poll(gw_poller_t *p, uint16_t dev, uint32_t now_ms)
{
    const gw_config_t *cfg = p->cfg;
    const gw_device_cfg_t *d = &cfg->devices[dev];
    bool online;
    gw_tagdb_get_device(cfg, dev, p->work, &online);

    /* A cycle fails as soon as a request stays unanswered after its retries (an exception
     * response is an answer). The remaining blocks are skipped, so the timeout is not spent
     * on each of them, and the tags of every block not read keep the value and timestamp of
     * their last read: nothing is reported for them as fresh. offline_after failed cycles in
     * a row declare the device offline (DDEATH, all values invalid), and only a complete
     * cycle brings it (back) online, so a DBIRTH never carries a value that was not read. */
    bool failed = false;
    bool probe = is_offline(p, dev);   /* first request: a single attempt (see gw_poll.h) */
    for (uint16_t b = 0; b < p->plan.dev_block_count[dev] && !failed; b++) {
        const gw_block_t *blk = &p->plan.blocks[p->plan.dev_first_block[dev] + b];
        mb_request_t rq = {
            .unit = d->unit_id, .fc = read_fc(blk->area), .addr = blk->start, .qty = blk->qty};
        mb_response_t rsp;
        mb_status_t st = execute_n(p, dev, &rq, &rsp, probe && b == 0 ? 0 : p->mb->cfg.retries);
        if (st != MB_OK && st != MB_ERR_EXCEPTION) {
            failed = true;
            GW_LOGD(TAG, "%s: fc %u addr %u qty %u unanswered (status %d); cycle failed", d->name,
                    rq.fc, rq.addr, rq.qty, st);
            continue;
        }
        uint64_t ts = gw_port_time_ms();
        if (st == MB_ERR_EXCEPTION) {
            GW_LOGW(TAG, "%s: exception %u reading fc %u addr %u qty %u", d->name, rsp.exception,
                    rq.fc, rq.addr, rq.qty);
        }
        for (uint16_t m = 0; m < blk->member_count; m++) {
            uint16_t tag = p->plan.members[blk->first_member + m];
            gw_tag_value_t *v = &p->work[tag - d->first_tag];
            v->ts_ms = ts;
            v->valid = st == MB_OK;
            if (v->valid) {
                decode_member(&cfg->tags[tag], &rsp, blk->start, v);
            }
        }
    }

    gw_dev_event_t ev = gw_devstate_cycle(&p->state[dev], !failed, d->offline_after);
    p->stats[dev].cycles++;
    p->stats[dev].failed_cycles += failed ? 1u : 0u;
    if (ev == GW_DEV_WENT_OFFLINE) {
        for (uint16_t i = 0; i < d->tag_count; i++) {
            p->work[i].valid = false;
        }
        const mb_stats_t *st = &p->stats[dev].bus;
        GW_LOGW(TAG,
                "%s (unit %u) offline after %u failed cycles; device totals: ok %lu timeout %lu "
                "crc %lu frame %lu exception %lu",
                d->name, d->unit_id, d->offline_after, (unsigned long)st->ok,
                (unsigned long)st->timeout, (unsigned long)st->crc, (unsigned long)st->frame,
                (unsigned long)st->exception);
    } else if (ev == GW_DEV_WENT_ONLINE) {
        GW_LOGI(TAG, "%s (unit %u) online", d->name, d->unit_id);
    }
    gw_tagdb_put_device(cfg, dev, p->work, p->state[dev].online);
    gw_tagdb_put_stats(dev, &p->stats[dev]);

    if (is_offline(p, dev)) {
        /* Offline: probe again after an exponential backoff, measured from this cycle. */
        uint32_t cap = d->poll_ms > p->opts.offline_probe_max_ms ? d->poll_ms
                                                                 : p->opts.offline_probe_max_ms;
        uint32_t next = p->probe_ms[dev] == 0 ? d->poll_ms : p->probe_ms[dev];
        next = next > cap / 2 ? cap : next * 2;
        p->probe_ms[dev] = next;
        p->next_due_ms[dev] = now_ms + next;
        GW_LOGD(TAG, "%s offline: next probe in %lu ms", d->name, (unsigned long)next);
        return ev;
    }
    p->probe_ms[dev] = 0;
    p->next_due_ms[dev] += d->poll_ms;
    if ((int32_t)(p->next_due_ms[dev] - now_ms) <= 0) {
        p->next_due_ms[dev] = now_ms + d->poll_ms;  /* overrun: skip, never burst to catch up */
    }
    return ev;
}

/* Request for a register bit (MB-6): read-modify-write of the whole register. The read and the
 * write go back to back through this task, so no other gateway traffic comes in between, but
 * the device itself may change the other bits in that window (documented limitation). */
static gw_write_status_t bit_request(gw_poller_t *p, uint16_t dev, const gw_tag_cfg_t *t, bool on,
                                     mb_request_t *rq, uint16_t *reg)
{
    const gw_device_cfg_t *d = &p->cfg->devices[dev];
    mb_request_t rd = {.unit = d->unit_id, .fc = MB_FC_READ_HOLDING_REGISTERS, .addr = t->address,
                       .qty = 1};
    mb_response_t rsp;
    mb_status_t st = execute(p, dev, &rd, &rsp);
    if (st != MB_OK) {
        GW_LOGW(TAG, "%s/%s: read of register %u for the bit write failed (status %d)", d->name,
                t->name, t->address, st);
        return GW_WRITE_BUS_ERROR;
    }
    uint16_t mask = (uint16_t)(1u << t->bit);
    uint16_t old = mb_rsp_reg(&rsp, 0);
    *reg = on ? (uint16_t)(old | mask) : (uint16_t)(old & ~mask);
    if (p->opts.force_multi_write) {
        rq->fc = MB_FC_WRITE_MULTIPLE_REGISTERS;
        rq->qty = 1;
        rq->regs = reg;
    } else {
        rq->fc = MB_FC_WRITE_SINGLE_REGISTER;
        rq->value = *reg;
    }
    return GW_WRITE_OK;
}

gw_write_status_t gw_poller_write_value(gw_poller_t *p, uint16_t tag, const gw_tag_value_t *value)
{
    const gw_config_t *cfg = p->cfg;
    uint16_t dev = gw_config_tag_device(cfg, tag);
    if (dev == GW_NO_DEVICE) {
        GW_LOGE(TAG, "write to tag %u: no such tag", tag);
        return GW_WRITE_UNKNOWN_TAG;
    }
    const gw_tag_cfg_t *t = &cfg->tags[tag];
    const gw_device_cfg_t *d = &cfg->devices[dev];
    if (!t->writable) {
        return GW_WRITE_NOT_WRITABLE;
    }
    uint16_t width = gw_tag_width(t);
    uint16_t regs[GW_TAG_MAX_REGS] = {0};
    uint8_t bits[1] = {0};
    mb_request_t rq = {.unit = d->unit_id, .addr = t->address};
    gw_tag_value_t confirmed;
    memset(&confirmed, 0, sizeof(confirmed));
    char text[48];

    if (t->type == GW_TYPE_BOOL) {
        /* NaN != 0.0 is true: without this check a NaN command would switch the coil ON. */
        if (!isfinite(value->value)) {
            GW_LOGW(TAG, "%s/%s: value %g not representable (not a finite number)", d->name,
                    t->name, value->value);
            return GW_WRITE_BAD_VALUE;
        }
        bool on = value->value != 0.0;
        confirmed.value = on ? 1.0 : 0.0;
        if (t->area == GW_AREA_HOLDING) {
            gw_write_status_t ws = bit_request(p, dev, t, on, &rq, &regs[0]);
            if (ws != GW_WRITE_OK) {
                gw_tagdb_put_stats(dev, &p->stats[dev]);
                return ws;
            }
        } else if (p->opts.force_multi_write) {
            bits[0] = on ? 1u : 0u;
            rq.fc = MB_FC_WRITE_MULTIPLE_COILS;
            rq.qty = 1;
            rq.bits = bits;
        } else {
            rq.fc = MB_FC_WRITE_SINGLE_COIL;
            rq.value = on;
        }
    } else {
        gw_conv_status_t cs = gw_conv_encode(t, value, regs);
        if (cs != GW_CONV_OK) {
            gw_conv_format(t, value, text, sizeof(text));
            GW_LOGW(TAG, "%s/%s: value %s not representable (%d)", d->name, t->name, text, cs);
            return GW_WRITE_BAD_VALUE;
        }
        gw_conv_decode(t, regs, &confirmed);
        if (width == 1 && !p->opts.force_multi_write) {
            rq.fc = MB_FC_WRITE_SINGLE_REGISTER;
            rq.value = regs[0];
        } else {
            rq.fc = MB_FC_WRITE_MULTIPLE_REGISTERS;
            rq.qty = width;
            rq.regs = regs;
        }
    }

    mb_response_t rsp;
    mb_status_t st = execute(p, dev, &rq, &rsp);
    gw_tagdb_put_stats(dev, &p->stats[dev]);
    if (st != MB_OK) {
        GW_LOGW(TAG, "%s/%s: write fc %u failed (status %d, exception %u)", d->name, t->name,
                rq.fc, st, st == MB_ERR_EXCEPTION ? rsp.exception : 0);
        return GW_WRITE_BUS_ERROR;
    }

    /* Read back so the published value is what the device actually holds. */
    mb_request_t rd = {.unit = d->unit_id, .fc = read_fc(t->area), .addr = t->address, .qty = width};
    bool read_back = execute(p, dev, &rd, &rsp) == MB_OK;
    gw_tagdb_put_stats(dev, &p->stats[dev]);
    if (read_back) {
        decode_member(t, &rsp, t->address, &confirmed);
    } else {
        GW_LOGW(TAG, "%s/%s: read-back failed, reporting the acknowledged value", d->name, t->name);
    }
    confirmed.ts_ms = gw_port_time_ms();
    confirmed.valid = true;
    gw_tagdb_put_tag(tag, &confirmed);
    gw_conv_format(t, &confirmed, text, sizeof(text));
    GW_LOGI(TAG, "%s/%s <- %s (fc %u)", d->name, t->name, text, rq.fc);
    return GW_WRITE_OK;
}

gw_write_status_t gw_poller_write(gw_poller_t *p, uint16_t tag, double value)
{
    uint16_t dev = gw_config_tag_device(p->cfg, tag);
    if (dev == GW_NO_DEVICE) {
        GW_LOGE(TAG, "write to tag %u: no such tag", tag);
        return GW_WRITE_UNKNOWN_TAG;
    }
    gw_tag_value_t v;
    if (!gw_conv_from_double(&p->cfg->tags[tag], value, &v)) {
        return GW_WRITE_BAD_VALUE;
    }
    return gw_poller_write_value(p, tag, &v);
}

uint16_t gw_poller_offline_count(const gw_poller_t *p)
{
    uint16_t n = 0;
    for (uint16_t d = 0; d < p->cfg->device_count; d++) {
        n = (uint16_t)(n + (p->state[d].online ? 0u : 1u));
    }
    return n;
}
