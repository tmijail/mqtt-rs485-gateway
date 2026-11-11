/*
 * Shell on the console (CFG-6): esp_console REPL with
 *   status        firmware, reset reason, configuration state, network / MQTT /
 *                 Modbus state, local I/O and heap
 *   devices       every Modbus device with its online state and counters
 *   config        summary of the loaded configuration
 *   config show   the stored document, secrets masked
 *   config load   paste a JSON document: validated, saved atomically, restart
 *   config check  paste a JSON document: validated only
 *   restart       orderly restart (NDEATH first)
 *   rebirth       re-issue NBIRTH and every DBIRTH
 *   mbread        one Modbus read through the bus owner (MB-16)
 *   ping          ICMP echo
 *   di, boot      (emulator builds only) set a simulated digital input / the BOOT button
 * The commands that change something can be compiled out (CONFIG_GW_SHELL_WRITE_COMMANDS).
 * The shell also runs when the configuration was rejected: then it is a way to load a
 * valid one without reflashing.
 */
#pragma once

#include "esp_err.h"
#include "gw_config.h"
#include "gw_node.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const gw_config_t *cfg;       /* NULL: the configuration was rejected (or is being loaded) */
    bool starting;                /* issue #72: the configuration is still being loaded */
    const char *config_source;
    const char *config_error;     /* reason when cfg is NULL */
    bool commissioning;           /* cfg is NULL because of a factory reset (CFG-5) */
    const char *fault;            /* #135: cfg is NULL because of a hardware fault (no PSRAM) */
    const gw_node_info_t *info;
    void (*restart)(void);        /* orderly restart (NDEATH first when a session is up) */
} gw_shell_ctx_t;

/** Start the REPL task, as early as possible (issue #72): input typed during the boot is
 *  answered, not lost. Everything @p ctx points to must outlive the gateway. */
esp_err_t gw_shell_start(const gw_shell_ctx_t *ctx);

/** Replace the context once the configuration is known (or rejected). Waits for a command in
 *  progress to finish. */
void gw_shell_set_ctx(const gw_shell_ctx_t *ctx);

/* Subsystems the commands need; until marked up a command that needs one answers "not
 * available yet" instead of touching it. */
#define GW_SHELL_UP_EDGE 0x01u   /* gw_edge (MQTT): restart, rebirth */
#define GW_SHELL_UP_BUS  0x02u   /* gw_bus: mbread */
#define GW_SHELL_UP_NET  0x04u   /* esp_netif: ping */
#define GW_SHELL_UP_IO   0x08u   /* gw_io inputs: di (emulator) */

void gw_shell_mark_up(unsigned what);

#ifdef __cplusplus
}
#endif
