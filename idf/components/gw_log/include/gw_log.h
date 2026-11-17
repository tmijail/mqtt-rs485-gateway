/*
 * Remote syslog (REL-4): copies every esp_log line to an RFC 5424 server over UDP.
 *
 * #200: the live log of the web UI. From the boot on, a ring in PSRAM keeps the last log lines
 * (CONFIG_GW_LOG_RING_KB) for GET /api/log. #199: the log level of each tag at run time
 * (esp_log_level_set): not saved, a restart sets the level of the build again.
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
#include "gw_logring.h"

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

/** #200: the ring of the live log (PSRAM), as early in the boot as possible, in every mode. */
esp_err_t gw_log_ring_start(void);

typedef struct {
    uint32_t from, last;    /* the numbers of the first and the last line copied (gw_logring) */
    uint32_t first;         /* the oldest line in the ring */
    uint32_t next;          /* the number of the next line */
    uint32_t lines;         /* lines in the ring */
    size_t used, size;      /* bytes */
    uint32_t dropped;       /* lines that the ring did not get (another task held it) */
} gw_log_ring_info_t;

/** #200: the lines after number @p since, as text (each line and a newline) in a buffer of
 *  gw_log that stays valid until the next call. One reader only (the web server task). False:
 *  no ring. */
bool gw_log_ring_copy(uint32_t since, const char **text, size_t *len, gw_log_ring_info_t *info);

/** #199: the tags of the tag table (the tags of the firmware and the tags that the log showed),
 *  with their changed levels. Returns the count. */
size_t gw_log_tags(gw_logtag_t *out, size_t cap);

/** #199: the tag table as JSON (gw_logtags_json): the changed levels, or every tag. "[]" when
 *  there is no table. Returns the length. */
size_t gw_log_tags_json(bool changed_only, char *out, size_t cap);

/** #199: the level of the build (CONFIG_LOG_DEFAULT_LEVEL) and the highest level that the build
 *  keeps (CONFIG_LOG_MAXIMUM_LEVEL), as gw_logring levels. */
int gw_log_build_level(void);
int gw_log_max_level(void);

typedef enum {
    GW_LOG_SET_OK = 0,
    GW_LOG_SET_BAD_TAG,     /* not 1 to 23 printable characters without spaces and ':' */
    GW_LOG_SET_BAD_LEVEL,   /* not a level, or above the maximum level of the build */
    GW_LOG_SET_NO_ROOM,     /* the tag table is full (or no ring) */
} gw_log_set_t;

/** #199: set the level of @p tag ("*": every tag without a level of its own). Not saved. */
gw_log_set_t gw_log_level_set(const char *tag, int level);

/** #199: every tag gets the level of the build again ("Reset all"). */
void gw_log_level_reset(void);

/** Network state (gw_net callback): the server is only resolved and contacted while up. */
void gw_log_notify_network(bool up);

void gw_log_get_stats(gw_log_stats_t *out);

#ifdef __cplusplus
}
#endif
