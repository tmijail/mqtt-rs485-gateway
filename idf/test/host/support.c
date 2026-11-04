#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_port.h"
#include "gw_tagdb.h"
#include "mb_rtu.h"
#include "spb.h"

/* ---------- files ---------- */

const char *support_read_file(const char *path, size_t *len)
{
    static char buf[64 * 1024];
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        abort();
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

const gw_config_t *support_spec_config(void)
{
    static gw_config_t cfg;
    size_t len;
    const char *json = support_read_file(GW_SPEC_EXAMPLE, &len);
    gw_cfg_err_t err;
    if (!gw_config_parse(json, len, &cfg, &err)) {
        fprintf(stderr, "spec example rejected: %s\n", err.msg);
        abort();
    }
    return &cfg;
}

/* ---------- simulated slave ---------- */

static uint8_t s_resp[MB_ADU_MAX];
static size_t s_resp_len;
static size_t s_resp_pos;

void sim_reset(sim_slave_t *s, uint8_t unit)
{
    memset(s, 0, sizeof(*s));
    s->unit = unit;
    s_resp_len = s_resp_pos = 0;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void respond(size_t len)
{
    uint16_t crc = mb_crc16(s_resp, len);
    s_resp[len] = (uint8_t)crc;
    s_resp[len + 1] = (uint8_t)(crc >> 8);
    s_resp_len = len + 2;
    s_resp_pos = 0;
}

static void respond_exception(uint8_t fc, uint8_t code)
{
    s_resp[1] = fc | 0x80u;
    s_resp[2] = code;
    respond(3);
}

static int sim_write(void *ctx, const uint8_t *d, size_t len)
{
    sim_slave_t *s = ctx;
    s->requests++;
    s_resp_len = s_resp_pos = 0;
    s->clock_us += len * 1146u;  /* ~ one 11-bit char at 9600 baud */
    if (len < 4 || mb_crc16(d, len - 2) != (uint16_t)(d[len - 2] | (d[len - 1] << 8))) {
        fprintf(stderr, "sim: master sent a bad CRC\n");
        abort();
    }
    if (s->offline || d[0] != s->unit || (s->mute_fc != 0 && d[1] == s->mute_fc)) {
        return 0;
    }
    uint8_t fc = d[1];
    uint16_t addr = rd16(&d[2]);
    uint16_t qty = rd16(&d[4]);
    s->last_fc = fc;
    if (fc == MB_FC_WRITE_SINGLE_COIL || fc == MB_FC_WRITE_SINGLE_REGISTER ||
        fc == MB_FC_WRITE_MULTIPLE_COILS || fc == MB_FC_WRITE_MULTIPLE_REGISTERS) {
        s->last_write_fc = fc;
    }
    s_resp[0] = s->unit;
    s_resp[1] = fc;
    if (s->exception) {
        respond_exception(fc, s->exception);
    } else if (fc == MB_FC_WRITE_SINGLE_COIL || fc == MB_FC_WRITE_SINGLE_REGISTER) {
        if (addr >= 256) {
            respond_exception(fc, 2);
        } else {
            if (fc == MB_FC_WRITE_SINGLE_COIL) {
                s->coils[addr] = qty == 0xFF00u;
            } else {
                s->holding[addr] = qty;
            }
            memcpy(s_resp, d, 6);
            respond(6);
        }
    } else if (addr + qty > 256) {
        respond_exception(fc, 2);
    } else if (fc == MB_FC_READ_COILS || fc == MB_FC_READ_DISCRETE_INPUTS) {
        const bool *src = fc == MB_FC_READ_COILS ? s->coils : s->discrete;
        uint8_t nb = (uint8_t)((qty + 7) / 8);
        memset(&s_resp[3], 0, nb);
        for (uint16_t i = 0; i < qty; i++) {
            if (src[addr + i]) {
                s_resp[3 + i / 8] |= (uint8_t)(1u << (i % 8));
            }
        }
        s_resp[2] = nb;
        respond(3u + nb);
    } else if (fc == MB_FC_READ_HOLDING_REGISTERS || fc == MB_FC_READ_INPUT_REGISTERS) {
        const uint16_t *src = fc == MB_FC_READ_HOLDING_REGISTERS ? s->holding : s->input;
        s_resp[2] = (uint8_t)(qty * 2);
        for (uint16_t i = 0; i < qty; i++) {
            s_resp[3 + 2 * i] = (uint8_t)(src[addr + i] >> 8);
            s_resp[4 + 2 * i] = (uint8_t)src[addr + i];
        }
        respond(3u + 2u * qty);
    } else if (fc == MB_FC_WRITE_MULTIPLE_COILS) {
        for (uint16_t i = 0; i < qty; i++) {
            s->coils[addr + i] = (d[7 + i / 8] >> (i % 8)) & 1u;
        }
        memcpy(s_resp, d, 6);
        respond(6);
    } else if (fc == MB_FC_WRITE_MULTIPLE_REGISTERS) {
        for (uint16_t i = 0; i < qty; i++) {
            s->holding[addr + i] = rd16(&d[7 + 2 * i]);
        }
        memcpy(s_resp, d, 6);
        respond(6);
    } else {
        respond_exception(fc, 1);
    }
    if (s_resp_len > 0 && s->wrong_unit > 0) {
        s->wrong_unit--;
        s_resp[0] = (uint8_t)(s->unit + 1);
        respond(s_resp_len - 2);
    }
    if (s_resp_len > 0 && s->crc_errors > 0) {
        s->crc_errors--;
        s_resp[s_resp_len - 1] ^= 0x5Au;
    }
    return 0;
}

static int sim_read(void *ctx, uint8_t *d, size_t len, uint32_t timeout_ms)
{
    sim_slave_t *s = ctx;
    size_t avail = s_resp_len - s_resp_pos;
    size_t n = avail < len ? avail : len;
    memcpy(d, &s_resp[s_resp_pos], n);
    s_resp_pos += n;
    s->clock_us += n < len ? (uint64_t)timeout_ms * 1000u : n * 1146u;
    return (int)n;
}

static void sim_flush(void *ctx)
{
    (void)ctx;
    s_resp_pos = s_resp_len;
}

static void sim_delay(void *ctx, uint32_t us)
{
    sim_slave_t *s = ctx;
    s->delay_total_us += us;
    s->clock_us += us;
}

static uint64_t sim_now(void *ctx)
{
    return ((sim_slave_t *)ctx)->clock_us;
}

mb_transport_t sim_transport(sim_slave_t *s)
{
    mb_transport_t t = {
        .write = sim_write,
        .read = sim_read,
        .flush = sim_flush,
        .delay_us = sim_delay,
        .now_us = sim_now,
        .on_attempt = NULL,
        .ctx = s,
    };
    return t;
}

/* ---------- recorder ---------- */

recorder_t g_rec;

static int rec_publish(void *ctx, const char *topic, const uint8_t *data, size_t len, int qos,
                       bool retain)
{
    (void)ctx;
    if (g_rec.count >= REC_MAX) {
        return -1;
    }
    rec_msg_t *m = &g_rec.msgs[g_rec.count++];
    snprintf(m->topic, sizeof(m->topic), "%s", topic);
    memcpy(m->payload, data, len);
    m->len = len;
    m->qos = qos;
    m->retain = retain;
    return 0;
}

static bool rec_set_do(void *ctx, uint8_t index, bool on)
{
    (void)ctx;
    if (on) {
        g_rec.dout |= (uint8_t)(1u << index);
    } else {
        g_rec.dout &= (uint8_t)~(1u << index);
    }
    gw_tagdb_set_do(g_rec.dout, gw_port_time_ms());
    return true;
}

static bool rec_request_write(void *ctx, uint16_t tag, double value)
{
    (void)ctx;
    if (g_rec.write_queue_full) {
        return false;
    }
    g_rec.writes++;
    if (g_rec.room_limited && g_rec.room > 0) {
        g_rec.room--;
    }
    g_rec.last_write_tag = tag;
    g_rec.last_write_value = value;
    return true;
}

static size_t rec_write_room(void *ctx)
{
    (void)ctx;
    return g_rec.room_limited ? g_rec.room : 64;
}

static void rec_reboot(void *ctx)
{
    (void)ctx;
    g_rec.reboots++;
}

gw_node_ops_t recorder_ops(void)
{
    gw_node_ops_t ops = {
        .publish = rec_publish,
        .set_do = rec_set_do,
        .request_write = rec_request_write,
        .write_room = rec_write_room,
        .reboot = rec_reboot,
        .ctx = NULL,
    };
    return ops;
}

void recorder_clear(void)
{
    memset(&g_rec, 0, sizeof(g_rec));
}

/* ---------- decoding ---------- */

static bool dec_cb(void *ctx, const spb_rx_metric_t *m)
{
    decoded_t *d = ctx;
    if (d->count >= DEC_MAX) {
        return false;
    }
    __typeof__(d->m[0]) *o = &d->m[d->count++];
    memset(o, 0, sizeof(*o));
    if (m->name) {
        o->has_name = true;
        snprintf(o->name, sizeof(o->name), "%s", m->name);
    }
    o->alias = m->alias;
    o->datatype = m->datatype;
    o->is_null = m->is_null;
    o->kind = m->kind;
    o->timestamp = m->timestamp;
    if (m->kind == SPB_VAL_STRING) {
        snprintf(o->str, sizeof(o->str), "%s", m->v.s);
    } else {
        spb_rx_number(m, m->datatype, &o->num);
    }
    return true;
}

bool decode_payload(const uint8_t *buf, size_t len, decoded_t *out)
{
    memset(out, 0, sizeof(*out));
    spb_rx_header_t hdr;
    if (!spb_decode(buf, len, &hdr, dec_cb, out)) {
        return false;
    }
    out->has_seq = hdr.has_seq;
    out->seq = hdr.seq;
    out->timestamp = hdr.timestamp;
    return true;
}

int decoded_find_alias(const decoded_t *d, uint64_t alias)
{
    for (size_t i = 0; i < d->count; i++) {
        if (d->m[i].alias == alias) {
            return (int)i;
        }
    }
    return -1;
}

int decoded_find_name(const decoded_t *d, const char *name)
{
    for (size_t i = 0; i < d->count; i++) {
        if (d->m[i].has_name && strcmp(d->m[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ---------- hand-encoded payloads ---------- */

static size_t put_varint(uint8_t *out, uint64_t v)
{
    size_t n = 0;
    do {
        out[n] = (uint8_t)(v & 0x7Fu);
        v >>= 7;
        if (v != 0) {
            out[n] |= 0x80u;
        }
        n++;
    } while (v != 0);
    return n;
}

static size_t put_bytes(uint8_t *out, uint8_t key, const void *data, size_t len)
{
    size_t n = 0;
    out[n++] = key;
    n += put_varint(&out[n], len);
    memcpy(&out[n], data, len);
    return n + len;
}

size_t raw_payload(uint8_t *out, size_t cap, const raw_metric_t *m, size_t count)
{
    static uint8_t metric[1024];
    size_t n = 0;
    out[n++] = 0x08;   /* timestamp = 1 */
    out[n++] = 0x01;
    for (size_t i = 0; i < count; i++) {
        size_t k = 0;
        if (m[i].name != NULL) {
            k += put_bytes(&metric[k], 0x0A, m[i].name, strlen(m[i].name));   /* 1: name */
        }
        if (m[i].has_alias) {
            metric[k++] = 0x10;                                               /* 2: alias */
            k += put_varint(&metric[k], m[i].alias);
        }
        metric[k++] = 0x20;                                                   /* 4: datatype */
        k += put_varint(&metric[k], m[i].datatype);
        if (m[i].kind == RAW_BOOL) {
            metric[k++] = 0x70;                                               /* 14: boolean */
            metric[k++] = m[i].num != 0.0 ? 1u : 0u;
        } else if (m[i].kind == RAW_FLOAT) {
            float f = (float)m[i].num;
            metric[k++] = 0x65;                                               /* 12: float */
            memcpy(&metric[k], &f, 4);
            k += 4;
        } else {
            k += put_bytes(&metric[k], 0x7A, m[i].str, strlen(m[i].str));    /* 15: string */
        }
        if (n + k + 8 > cap) {
            fprintf(stderr, "raw_payload: buffer too small\n");
            abort();
        }
        n += put_bytes(&out[n], 0x12, metric, k);                             /* 2: metrics */
    }
    return n;
}
