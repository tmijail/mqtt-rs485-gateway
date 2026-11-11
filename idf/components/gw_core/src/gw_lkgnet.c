#include "gw_lkgnet.h"

#include <string.h>

/* The tags of the fields. Never reuse a number: an earlier image reads it with its old meaning. */
enum {
    T_HOSTNAME = 1,
    T_MDNS = 2,
    T_DHCP = 3,
    T_IP = 4,
    T_NETMASK = 5,
    T_GATEWAY = 6,
    T_DNS = 7,
    T_V6_ENABLED = 8,
    T_V6_SLAAC = 9,
    T_V6_ADDRESS = 10,   /* present: has_address */
    T_V6_DNS = 11,       /* present: has_dns */
    T_WIFI_ENABLED = 12,
    T_WIFI_SSID = 13,
    T_WIFI_PASSWORD = 14,
    T_WIFI_ROLE = 15,
    T_WIFI_FAILBACK = 16,
    T_WIFI_DHCP = 17,
    T_WIFI_IP = 18,
    T_WIFI_NETMASK = 19,
    T_WIFI_GATEWAY = 20,
    T_WIFI_DNS = 21,
    T_WEB_PORT = 30,
    T_WEB_HTTPS_PORT = 31,
    T_WEB_HTTPS = 32,
};

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t n;
    bool ok;
} writer_t;

static void put(writer_t *w, uint8_t tag, const void *data, size_t len)
{
    if (!w->ok || len > 255u || w->n + 2u + len > w->cap) {
        w->ok = false;
        return;
    }
    w->buf[w->n++] = tag;
    w->buf[w->n++] = (uint8_t)len;
    memcpy(w->buf + w->n, data, len);
    w->n += len;
}

static void put_u8(writer_t *w, uint8_t tag, unsigned v)
{
    uint8_t b = (uint8_t)v;
    put(w, tag, &b, 1);
}

static void put_u16(writer_t *w, uint8_t tag, unsigned v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(w, tag, b, 2);
}

static void put_str(writer_t *w, uint8_t tag, const char *s)
{
    put(w, tag, s, strnlen(s, 255));
}

size_t gw_lkg_encode(const gw_config_t *cfg, uint8_t *buf, size_t cap)
{
    writer_t w = {.buf = buf, .cap = cap, .n = 0, .ok = cap >= 5};
    if (!w.ok) {
        return 0;
    }
    memcpy(buf, GW_LKG_MAGIC, 4);
    buf[4] = GW_LKG_VERSION;
    w.n = 5;
    __typeof__(cfg->network) *n = &cfg->network;   /* const: cfg is const */
    put_str(&w, T_HOSTNAME, n->hostname);
    put_u8(&w, T_MDNS, n->mdns);
    put_u8(&w, T_DHCP, n->dhcp);
    put(&w, T_IP, n->ip, 4);
    put(&w, T_NETMASK, n->netmask, 4);
    put(&w, T_GATEWAY, n->gateway, 4);
    put(&w, T_DNS, n->dns, 4);
    put_u8(&w, T_V6_ENABLED, n->ipv6.enabled);
    put_u8(&w, T_V6_SLAAC, n->ipv6.slaac);
    if (n->ipv6.has_address) {
        put(&w, T_V6_ADDRESS, n->ipv6.address, 16);
    }
    if (n->ipv6.has_dns) {
        put(&w, T_V6_DNS, n->ipv6.dns, 16);
    }
    put_u8(&w, T_WIFI_ENABLED, n->wifi.enabled);
    put_str(&w, T_WIFI_SSID, n->wifi.ssid);
    put_str(&w, T_WIFI_PASSWORD, n->wifi.password);
    put_u8(&w, T_WIFI_ROLE, (unsigned)n->wifi.role);
    put_u16(&w, T_WIFI_FAILBACK, n->wifi.failback_s);
    put_u8(&w, T_WIFI_DHCP, n->wifi.ipv4.dhcp);
    put(&w, T_WIFI_IP, n->wifi.ipv4.ip, 4);
    put(&w, T_WIFI_NETMASK, n->wifi.ipv4.netmask, 4);
    put(&w, T_WIFI_GATEWAY, n->wifi.ipv4.gateway, 4);
    put(&w, T_WIFI_DNS, n->wifi.ipv4.dns, 4);
    put_u16(&w, T_WEB_PORT, cfg->web.port);
    put_u16(&w, T_WEB_HTTPS_PORT, cfg->web.https_port);
    put_u8(&w, T_WEB_HTTPS, cfg->web.https);
    return w.ok ? w.n : 0;
}

static void get_bytes(uint8_t *out, size_t size, const uint8_t *v, size_t len)
{
    if (len == size) {
        memcpy(out, v, size);
    }
}

static void get_str(char *out, size_t cap, const uint8_t *v, size_t len)
{
    if (len < cap && memchr(v, '\0', len) == NULL) {
        memcpy(out, v, len);
        out[len] = '\0';
    }
}

static void get_bool(bool *out, const uint8_t *v, size_t len)
{
    if (len == 1) {
        *out = v[0] != 0;
    }
}

static void get_u16(uint16_t *out, const uint8_t *v, size_t len)
{
    if (len == 2) {
        *out = (uint16_t)(v[0] | v[1] << 8);
    }
}

bool gw_lkg_decode(const uint8_t *buf, size_t len, gw_config_t *cfg)
{
    if (len < 5 || memcmp(buf, GW_LKG_MAGIC, 4) != 0 || buf[4] == 0) {
        return false;
    }
    /* first a pass that checks the framing: a cut record set changes nothing */
    for (size_t i = 5; i < len;) {
        if (i + 2u > len || i + 2u + buf[i + 1] > len) {
            return false;
        }
        i += 2u + buf[i + 1];
    }
    __typeof__(cfg->network) *n = &cfg->network;
    for (size_t i = 5; i < len;) {
        uint8_t tag = buf[i];
        size_t l = buf[i + 1];
        const uint8_t *v = buf + i + 2;
        i += 2u + l;
        switch (tag) {
        case T_HOSTNAME: get_str(n->hostname, sizeof(n->hostname), v, l); break;
        case T_MDNS: get_bool(&n->mdns, v, l); break;
        case T_DHCP: get_bool(&n->dhcp, v, l); break;
        case T_IP: get_bytes(n->ip, 4, v, l); break;
        case T_NETMASK: get_bytes(n->netmask, 4, v, l); break;
        case T_GATEWAY: get_bytes(n->gateway, 4, v, l); break;
        case T_DNS: get_bytes(n->dns, 4, v, l); break;
        case T_V6_ENABLED: get_bool(&n->ipv6.enabled, v, l); break;
        case T_V6_SLAAC: get_bool(&n->ipv6.slaac, v, l); break;
        case T_V6_ADDRESS:
            if (l == 16) {
                memcpy(n->ipv6.address, v, 16);
                n->ipv6.has_address = true;
            }
            break;
        case T_V6_DNS:
            if (l == 16) {
                memcpy(n->ipv6.dns, v, 16);
                n->ipv6.has_dns = true;
            }
            break;
        case T_WIFI_ENABLED: get_bool(&n->wifi.enabled, v, l); break;
        case T_WIFI_SSID: get_str(n->wifi.ssid, sizeof(n->wifi.ssid), v, l); break;
        case T_WIFI_PASSWORD: get_str(n->wifi.password, sizeof(n->wifi.password), v, l); break;
        case T_WIFI_ROLE:
            if (l == 1 && v[0] <= (uint8_t)GW_WIFI_PRIMARY) {
                n->wifi.role = (gw_wifi_role_t)v[0];
            }
            break;
        case T_WIFI_FAILBACK: get_u16(&n->wifi.failback_s, v, l); break;
        case T_WIFI_DHCP: get_bool(&n->wifi.ipv4.dhcp, v, l); break;
        case T_WIFI_IP: get_bytes(n->wifi.ipv4.ip, 4, v, l); break;
        case T_WIFI_NETMASK: get_bytes(n->wifi.ipv4.netmask, 4, v, l); break;
        case T_WIFI_GATEWAY: get_bytes(n->wifi.ipv4.gateway, 4, v, l); break;
        case T_WIFI_DNS: get_bytes(n->wifi.ipv4.dns, 4, v, l); break;
        case T_WEB_PORT: get_u16(&cfg->web.port, v, l); break;
        case T_WEB_HTTPS_PORT: get_u16(&cfg->web.https_port, v, l); break;
        case T_WEB_HTTPS: get_bool(&cfg->web.https, v, l); break;
        default: break;   /* a field of a newer image */
        }
    }
    return true;
}
