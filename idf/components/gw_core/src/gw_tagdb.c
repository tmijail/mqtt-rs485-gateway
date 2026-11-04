#include "gw_tagdb.h"

#include <string.h>

#include "gw_port.h"

static gw_tag_value_t s_tags[GW_MAX_TAGS];
static bool s_online[GW_MAX_DEVICES];
static gw_dev_stats_t s_stats[GW_MAX_DEVICES];
static gw_local_io_t s_io;

void gw_tagdb_reset(void)
{
    gw_port_lock();
    memset(s_tags, 0, sizeof(s_tags));
    memset(s_online, 0, sizeof(s_online));
    memset(s_stats, 0, sizeof(s_stats));
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

void gw_tagdb_get_stats(uint16_t dev, gw_dev_stats_t *stats)
{
    gw_port_lock();
    *stats = s_stats[dev];
    gw_port_unlock();
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

void gw_tagdb_set_do(uint8_t dout, uint64_t ts_ms)
{
    gw_port_lock();
    s_io.dout = dout;
    s_io.do_unknown = false;
    s_io.do_ts_ms = ts_ms;
    gw_port_unlock();
}

void gw_tagdb_set_do_unknown(uint64_t ts_ms)
{
    gw_port_lock();
    s_io.dout = 0;
    s_io.do_unknown = true;
    s_io.do_ts_ms = ts_ms;
    gw_port_unlock();
}

void gw_tagdb_get_io(gw_local_io_t *io)
{
    gw_port_lock();
    *io = s_io;
    gw_port_unlock();
}
