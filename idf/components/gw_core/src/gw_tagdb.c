#include "gw_tagdb.h"

#include <string.h>

#include "gw_port.h"

static gw_tag_value_t s_tags[GW_MAX_TAGS];
static bool s_online[GW_MAX_DEVICES];
static gw_dev_stats_t s_stats[GW_MAX_DEVICES];
static uint32_t s_refused[GW_MAX_DEVICES];   /* #196: kept apart from s_stats (the poll engine
                                                * writes those, the Sparkplug task these) */
static gw_local_io_t s_io;
static uint8_t s_fast;   /* #218: DIs that the PCNT counts (not their debounced activations) */

void gw_tagdb_reset(void)
{
    gw_port_lock();
    memset(s_tags, 0, sizeof(s_tags));
    memset(s_online, 0, sizeof(s_online));
    memset(s_stats, 0, sizeof(s_stats));
    memset(s_refused, 0, sizeof(s_refused));
    memset(&s_io, 0, sizeof(s_io));
    gw_port_unlock();
}

void gw_tagdb_put_device(const gw_config_t *cfg, uint16_t dev, const gw_tag_value_t *vals,
                         bool online)
{
    const gw_device_cfg_t *d = &cfg->devices[dev];
    gw_port_lock();
    memcpy(&s_tags[d->first_tag], vals, d->tag_count * sizeof(*vals));
    s_online[dev] = online;
    gw_port_unlock();
}

void gw_tagdb_get_device(const gw_config_t *cfg, uint16_t dev, gw_tag_value_t *vals, bool *online)
{
    const gw_device_cfg_t *d = &cfg->devices[dev];
    gw_port_lock();
    memcpy(vals, &s_tags[d->first_tag], d->tag_count * sizeof(*vals));
    *online = s_online[dev];
    gw_port_unlock();
}

void gw_tagdb_put_stats(uint16_t dev, const gw_dev_stats_t *stats)
{
    gw_port_lock();
    s_stats[dev] = *stats;
    gw_port_unlock();
}

bool gw_tagdb_device_online(uint16_t dev)
{
    gw_port_lock();
    bool online = s_online[dev];
    gw_port_unlock();
    return online;
}

void gw_tagdb_get_stats(uint16_t dev, gw_dev_stats_t *stats)
{
    gw_port_lock();
    *stats = s_stats[dev];
    gw_port_unlock();
}

void gw_tagdb_count_refused(uint16_t dev)
{
    if (dev >= GW_MAX_DEVICES) {
        return;
    }
    gw_port_lock();
    s_refused[dev]++;
    gw_port_unlock();
}

uint32_t gw_tagdb_refused(uint16_t dev)
{
    if (dev >= GW_MAX_DEVICES) {
        return 0;
    }
    gw_port_lock();
    uint32_t n = s_refused[dev];
    gw_port_unlock();
    return n;
}

void gw_tagdb_put_tag(uint16_t tag, const gw_tag_value_t *val)
{
    gw_port_lock();
    s_tags[tag] = *val;
    gw_port_unlock();
}

void gw_tagdb_get_tag(uint16_t tag, gw_tag_value_t *val)
{
    gw_port_lock();
    *val = s_tags[tag];
    gw_port_unlock();
}

void gw_tagdb_set_di(uint8_t di, uint64_t ts_ms)
{
    gw_port_lock();
    s_io.di = di;
    s_io.di_ts_ms = ts_ms;
    gw_port_unlock();
}

void gw_tagdb_update_di(uint8_t di, uint64_t ts_ms)
{
    gw_port_lock();
    uint8_t rising = (uint8_t)(di & ~s_io.di & ~s_fast);
    if (rising != 0) {
        s_io.count_ts_ms = ts_ms;
    }
    for (int i = 0; rising != 0; i++, rising >>= 1) {
        s_io.count[i] += rising & 1u;
    }
    s_io.di = di;
    s_io.di_ts_ms = ts_ms;
    gw_port_unlock();
}

void gw_tagdb_set_fast(uint8_t mask)
{
    gw_port_lock();
    s_fast = mask;
    gw_port_unlock();
}

void gw_tagdb_add_count(uint8_t index, uint32_t n, uint64_t ts_ms)
{
    if (n == 0 || index >= 8) {
        return;
    }
    gw_port_lock();
    s_io.count[index] += n;
    s_io.count_ts_ms = ts_ms;
    gw_port_unlock();
}

void gw_tagdb_set_run_hours(const uint64_t run_s[GW_RUN_HOURS_CHANNELS], uint64_t ts_ms)
{
    gw_port_lock();
    memcpy(s_io.run_s, run_s, sizeof(s_io.run_s));
    s_io.run_ts_ms = ts_ms;
    gw_port_unlock();
}

void gw_tagdb_set_counters(const uint64_t count[8])
{
    gw_port_lock();
    memcpy(s_io.count, count, sizeof(s_io.count));
    gw_port_unlock();
}

void gw_tagdb_set_do(uint8_t dout, uint64_t ts_ms)
{
    gw_port_lock();
    uint8_t rise = (uint8_t)(dout & ~s_io.dout);
    if (rise != 0) {
        s_io.do_rise |= rise;
        s_io.do_rise_ts_ms = ts_ms;
    }
    s_io.dout = dout;
    s_io.do_unknown = false;
    s_io.do_ts_ms = ts_ms;
    gw_port_unlock();
}

uint8_t gw_tagdb_take_do_rise(uint64_t *ts_ms)
{
    gw_port_lock();
    uint8_t rise = s_io.do_rise;
    *ts_ms = s_io.do_rise_ts_ms;
    s_io.do_rise = 0;
    gw_port_unlock();
    return rise;
}

void gw_tagdb_set_do_unknown(uint64_t ts_ms)
{
    gw_port_lock();
    s_io.dout = 0;
    s_io.do_unknown = true;
    s_io.do_ts_ms = ts_ms;
    gw_port_unlock();
}

void gw_tagdb_set_failsafe(bool on)
{
    gw_port_lock();
    s_io.failsafe = on;
    gw_port_unlock();
}

void gw_tagdb_get_io(gw_local_io_t *io)
{
    gw_port_lock();
    *io = s_io;
    gw_port_unlock();
}
