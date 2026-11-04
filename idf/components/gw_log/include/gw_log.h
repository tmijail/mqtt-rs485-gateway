/*
 * Remote syslog (REL-4): copies every esp_log line to an RFC 5424 server over UDP.
 *
 * The console output is unchanged. Logging tasks never block on the network: a line is
 * formatted into a bounded ring only if the formatting lock is free and the ring has room,
 * otherwise it is dropped and counted; a low-priority task sends the ring. Lines logged by
 * that task itself are not forwarded (no recursion).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    bool connected;      /* address resolved and socket open */
    uint32_t sent;
    uint32_t dropped;    /* lines lost: buffer full or another task formatting */
    uint32_t send_errors;
} gw_log_stats_t;

/** Start forwarding if cfg->syslog.host is set (no-op otherwise). @p cfg must outlive it. */
esp_err_t gw_log_start(const gw_config_t *cfg);

/** Network state (gw_net callback): the server is only resolved and contacted while up. */
void gw_log_notify_network(bool up);

void gw_log_get_stats(gw_log_stats_t *out);

#ifdef __cplusplus
}
#endif
