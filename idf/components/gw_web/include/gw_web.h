/*
 * Web UI and REST JSON API (CFG-3, CFG-4) on esp_http_server:
 *
 *   GET  /                      web UI (one embedded page): live status, configuration forms,
 *                               JSON editor, import and export
 *   GET  /api/status            live status (JSON)
 *   GET  /api/config            stored document, secrets masked (export; ?download=1 for a file)
 *   PUT  /api/config            replace it (import): validated, saved atomically, restart
 *   POST /api/config            same as PUT (for browsers and simple clients)
 *        ?dry_run=1             validate only
 *   POST /api/restart           orderly restart
 *   GET  /api/firmware          FW-1: running version, slot, update state and progress
 *   POST /api/firmware          FW-1: {"url": "https://..."} (application/json) downloads an
 *                               image; any other content type uploads the body as the image
 *   POST /api/card/format       #139: format the microSD card (?confirm=1)
 *   POST /api/rebirth           #171: NBIRTH and every DBIRTH again, same bdSeq (202; 409
 *                               without a Sparkplug session)
 *
 * Every request needs HTTP Basic auth: the configured web.username and web.password, or,
 * while no web password is set, user "admin" with the device's commissioning password.
 * Errors are JSON: {"ok":false,"error":"...","path":"devices[0].tags[1].type","line":0,
 * "column":0}. The server task is not on the critical path (priority 3, not watched).
 *
 * The server also runs when the configuration was rejected (recovery) and in commissioning
 * mode, with the network of the last valid configuration or DHCP on Ethernet; then the
 * stored web password is accepted with any user name.
 */
#pragma once

#include "esp_err.h"
#include "gw_config.h"
#include "gw_node.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_WEB_NORMAL = 0,     /* valid configuration, gateway running */
    GW_WEB_RECOVERY,       /* configuration rejected: nothing runs but network, web, shell */
    GW_WEB_COMMISSIONING,  /* after a factory reset: no configuration yet (CFG-5) */
} gw_web_mode_t;

typedef struct {
    gw_web_mode_t mode;
    const gw_config_t *cfg;              /* running configuration (network and web only in
                                          * recovery and commissioning) */
    const char *config_source;
    const gw_cfg_err_t *config_error;    /* why it was rejected (recovery) */
    const gw_node_info_t *info;
    const char *commissioning_password;  /* accepted for "admin" while no web password is set */
    void (*restart)(void);               /* orderly restart, after the response went out */
} gw_web_ctx_t;

/** Issue #64: login counters for the shell. */
typedef struct {
    bool running;
    bool https;            /* HTTPS with the device certificate (HTTP only redirects) */
    uint32_t failed;       /* failed logins */
    uint32_t refused;      /* requests refused while their address was blocked */
    uint32_t blocks;       /* times an address was blocked */
    unsigned blocked_now;
    uint32_t csrf_refused; /* issue #100: writes refused as cross-site (no header, other origin) */
    uint32_t cut_bodies;   /* issue #106: refused requests whose body was cut, not drained */
    uint32_t stalls;       /* issue #110: times a stalled server's sessions were shut down */
} gw_web_stats_t;

void gw_web_get_stats(gw_web_stats_t *out);

/** Issue #110: lowest free stack since boot (B) of the web server's task and of the HTTP
 *  redirect server's task (both are named "httpd"); 0 = not running (yet). */
void gw_web_stack_free(unsigned *web, unsigned *redirect);

/** Start the server: HTTPS on cfg->web.https_port (and a redirect on cfg->web.port) with
 *  cfg->web.https, else HTTP on cfg->web.port. Everything @p ctx points to must outlive it. */
esp_err_t gw_web_start(const gw_web_ctx_t *ctx);

/**
 * NET-6: answer <cfg->network.hostname>.local over mDNS and announce the web server as an
 * _http._tcp service (instance "<group>/<edge node>" or "gateway setup"), on every interface
 * (Ethernet, Wi-Fi station, access point). No-op with network.mdns false or without
 * CONFIG_GW_MDNS.
 */
esp_err_t gw_web_mdns_start(const gw_config_t *cfg, bool configured);

#ifdef __cplusplus
}
#endif
