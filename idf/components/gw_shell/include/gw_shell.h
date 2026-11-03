/*
 * Diagnostic shell on the console (CFG-6 subset): esp_console REPL with
 *   status    firmware, reset reason, configuration state, network / MQTT /
 *             Modbus state, local I/O and heap
 *   devices   every Modbus device with its online state and counters
 *   config    summary of the loaded configuration
 *   di        (emulator builds only) set a simulated digital input
 * It only reads shared state (gw_tagdb, gw_status) and changes nothing on the
 * bus or the MQTT session. It also runs when the configuration was rejected.
 */
#pragma once

#include "esp_err.h"
#include "gw_config.h"
#include "gw_node.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const gw_config_t *cfg;       /* NULL: the configuration was rejected */
    const char *config_source;
    const char *config_error;     /* reason when cfg is NULL */
    const gw_node_info_t *info;
} gw_shell_ctx_t;

/** Start the REPL task. Everything @p ctx points to must outlive the gateway. */
esp_err_t gw_shell_start(const gw_shell_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
