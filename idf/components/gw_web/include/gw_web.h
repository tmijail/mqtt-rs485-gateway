/*
 * Web server (esp_http_server) with the REST JSON API for the configuration (CFG-4):
 *
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
 */
#pragma once

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const gw_config_t *cfg;              /* running configuration */
    const char *commissioning_password;  /* accepted for "admin" while no web password is set */
    void (*restart)(void);               /* orderly restart, after the response went out */
} gw_web_ctx_t;

/** Start the server on cfg->web.port. Everything @p ctx points to must outlive it. */
esp_err_t gw_web_start(const gw_web_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
