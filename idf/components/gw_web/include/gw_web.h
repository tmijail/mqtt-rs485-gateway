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

/** Start the server on cfg->web.port. Everything @p ctx points to must outlive it. */
esp_err_t gw_web_start(const gw_web_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
