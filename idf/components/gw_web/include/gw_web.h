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
 *   POST /api/modbus/read       #182: one Modbus read through the bus owner (gw_mbdiag.h);
 *                               409 in recovery and commissioning (no bus)
 *   GET  /claim                 #120: the page of the claim window (no credential)
 *   GET  /api/claim             #120: the state of the claim window for this client
 *   POST /api/claim             #120: {"password": "..."}: the first web password, without a
 *                               credential, only while the claim window is open and only from
 *                               a client of the access point (gw_claim.h); CSRF check and the
 *                               login block of #64. Issue #187: the answer gives the SSID, the
 *                               plant addresses and the mDNS name for the success page
 *   POST /api/ap/stop           #187: stop the access point a short time after the answer (202;
 *                               409 when it does not run)
 *
 * Every other request needs HTTP Basic auth: the configured web.username and web.password, or,
 * while no web password is set, user "admin" with the device's commissioning password.
 * Errors are JSON: {"ok":false,"error":"...","path":"devices[0].tags[1].type","line":0,
 * "column":0}. The server task is not on the critical path (priority 3, not watched).
 *
 * The server also runs when the configuration was rejected (recovery), in commissioning
 * mode and in the fault mode (#135: no PSRAM), with the network of the last valid
 * configuration or DHCP on Ethernet; then the stored web password is accepted with any user
 * name.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"
#include "sdkconfig.h"
#include "gw_node.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_WEB_NORMAL = 0,     /* valid configuration, gateway running */
    GW_WEB_RECOVERY,       /* configuration rejected: nothing runs but network, web, shell */
    GW_WEB_COMMISSIONING,  /* after a factory reset: no configuration yet (CFG-5) */
    GW_WEB_FAULT,          /* #135: a hardware fault (no PSRAM): the gateway does not start;
                            * network, web and shell only, to report it */
} gw_web_mode_t;

typedef struct {
    gw_web_mode_t mode;
    const gw_config_t *cfg;              /* running configuration (network and web only in
                                          * recovery and commissioning) */
    const char *config_source;
    const gw_cfg_err_t *config_error;    /* why it was rejected (recovery) */
    const char *fault;                   /* #135: the hardware fault (GW_WEB_FAULT), or NULL */
    const gw_node_info_t *info;
    const char *commissioning_password;  /* accepted for "admin" while no web password is set */
    void (*restart)(void);               /* orderly restart, after the response went out */
    /* #120: the IPv4 address and netmask of the running access point (gw_ap_ip4), for the
     * access point rule of the claim window */
    bool (*ap_ip4)(uint8_t ip[4], uint8_t mask[4]);
    /* #120: the claim window closed (end of its time, or @p claimed: a password was set
     * through it). From the server task or from the esp_timer task: keep it short. */
    void (*claim_closed)(bool claimed);
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
    bool password_set;     /* issue #120: a web password is set (else the commissioning one) */
    bool claim_open;       /* issue #120: the claim window is open ... */
    uint32_t claim_left_s; /* ... for this many seconds more */
    uint32_t claim_refused;   /* issue #120: claims refused (closed, not from the AP, ...) */
} gw_web_stats_t;

void gw_web_get_stats(gw_web_stats_t *out);

/** Issue #144 item 1: the web server is running and its loop answered the watchdog's ping (or a
 *  handler ran) in the last @p max_age_ms. False when no server runs. */
bool gw_web_loop_alive(uint32_t max_age_ms);

/** Issue #110: lowest free stack since boot (B) of the web server's task and of the HTTP
 *  redirect server's task (both are named "httpd"); 0 = not running (yet). */
void gw_web_stack_free(unsigned *web, unsigned *redirect);

/** Start the server: HTTPS on cfg->web.https_port (and a redirect on cfg->web.port) with
 *  cfg->web.https, else HTTP on cfg->web.port. Everything @p ctx points to must outlive it. */
esp_err_t gw_web_start(const gw_web_ctx_t *ctx);

/**
 * CFG-3 / NET-5, issue #120: the time of a claim window (s): CONFIG_GW_WEB_CLAIM_WINDOW_MIN, or
 * in the emulator the time that the shell gave (gw_web_sim_claim). 0 when no window can open:
 * the server does not run, or a web password is set.
 */
uint32_t gw_web_claim_window_s(void);

/**
 * Issue #120: open the claim window for @p seconds (the BOOT gesture), or start its time again.
 * False (nothing opens) when the server does not run, a web password is set, or @p seconds is 0.
 * From any task.
 */
bool gw_web_claim_open(uint32_t seconds);

/**
 * Issue #187: close the claim window now, if it is open (the access point stopped: the window
 * accepts only its clients). @p why goes to the log. The claim_closed hook gets false. From any
 * task, but not from the claim_closed hook itself.
 */
void gw_web_claim_close(const char *why);

#if CONFIG_GW_IO_SIMULATED
/**
 * Emulator only (issue #120): the next claim windows last @p window_s seconds (0 = the Kconfig
 * time); with @p eth_as_ap the Ethernet interface counts as the access point (QEMU has no
 * radio), so the E2E can use the window over the emulated Ethernet. Without it, a claim over
 * Ethernet is refused as it is on the board.
 */
void gw_web_sim_claim(uint32_t window_s, bool eth_as_ap);
#endif

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
