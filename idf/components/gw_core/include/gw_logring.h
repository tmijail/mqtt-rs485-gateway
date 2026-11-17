/*
 * Issues #200 and #199: the live log of the web UI and the log level of each tag. Pure C
 * (host-tested); the IDF glue (gw_log) serialises the callers, owns the buffer (PSRAM) and calls
 * esp_log_level_set().
 *
 * The ring keeps the last log lines as text in one buffer: each line is followed by '\n'. A new
 * line removes the oldest lines until it fits. Each line has a sequence number: the first line
 * since the boot is 1, and the numbers have no gaps. A reader asks for the lines after the last
 * number that it has (GET /api/log?since=n); when the oldest of those lines are gone, it knows
 * how many it lost.
 *
 * The tag table holds the tags that the web UI offers for the log level: the tags of the
 * firmware (given at the start) and the tags that the log lines show, up to GW_LOGTAG_MAX. It also
 * holds the level of each tag that is not the default of the build (the changed levels; a restart
 * sets them all to the default again, because esp_log keeps them in RAM only).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_LOGRING_LINE_MAX 255   /* a longer line is cut */

typedef struct {
    char *buf;
    size_t size;        /* bytes of buf */
    size_t head;        /* offset of the oldest line */
    size_t used;        /* bytes in use (lines and their '\n') */
    uint32_t lines;     /* lines in the buffer */
    uint32_t next;      /* the number of the next line (1 for the first) */
} gw_logring_t;

void gw_logring_init(gw_logring_t *r, char *buf, size_t size);

/** Keep one line of @p len bytes. ANSI color sequences are removed, a '\r' or '\n' at the end is
 *  removed, and the other control characters become spaces. An empty line is not kept. */
void gw_logring_put(gw_logring_t *r, const char *line, size_t len);

/** The number of the oldest line in the buffer (the next number when it is empty). */
uint32_t gw_logring_first(const gw_logring_t *r);

/**
 * Copy the lines after number @p since into @p out, each one followed by '\n', in whole lines
 * only, until @p cap. @p from: the number of the first line copied; @p last: the number of the
 * last one (both @p since when no line was copied). Returns the bytes copied.
 */
size_t gw_logring_copy(const gw_logring_t *r, uint32_t since, char *out, size_t cap,
                       uint32_t *from, uint32_t *last);

/** The tag of a log line "I (123) tag: text" (with or without ANSI colors), false when the line
 *  does not have that shape. */
bool gw_logring_tag(const char *line, size_t len, const char **tag, size_t *tag_len);

/* ---- the log levels: the values of esp_log_level_t ---- */

#define GW_LOGLV_NONE    0
#define GW_LOGLV_ERROR   1
#define GW_LOGLV_WARN    2
#define GW_LOGLV_INFO    3
#define GW_LOGLV_DEBUG   4
#define GW_LOGLV_VERBOSE 5

/** "none", "error", "warn", "info", "debug", "verbose" ("?" for another value). */
const char *gw_loglevel_name(int level);

/** The level of @p name, or -1. */
int gw_loglevel_parse(const char *name);

/* ---- the tag table ---- */

#define GW_LOGTAG_MAX  48
#define GW_LOGTAG_SIZE 24   /* with the NUL; a longer tag is not kept ("*" is the default) */
#define GW_LOGTAG_DEFAULT (-1)

typedef struct {
    char name[GW_LOGTAG_SIZE];
    int8_t level;       /* GW_LOGTAG_DEFAULT: the level of the build */
} gw_logtag_t;

typedef struct {
    gw_logtag_t tag[GW_LOGTAG_MAX];
    unsigned count;
} gw_logtags_t;

void gw_logtags_init(gw_logtags_t *t);

/** Add the tag (@p len bytes) when it is not in the table and the table has room. Returns the
 *  entry, or NULL (no room, empty or too long). */
gw_logtag_t *gw_logtags_add(gw_logtags_t *t, const char *name, size_t len);

/**
 * Record the level of @p name ("*": the level of the tags that have no level of their own, as
 * esp_log_level_set("*") does). A level equal to @p build_default makes the entry default again.
 * False when the table has no room for a new tag (nothing changed).
 */
bool gw_logtags_set(gw_logtags_t *t, const char *name, int level, int build_default);

/** Make every entry default again ("Reset all"; the glue sets each changed tag first). */
void gw_logtags_reset_all(gw_logtags_t *t);

/** The tags whose level is not the default, in table order. Returns the count. */
size_t gw_logtags_changed(const gw_logtags_t *t, const gw_logtag_t **out, size_t cap);

/** JSON of the table: @p changed_only: [{"tag":"poll","level":"debug"}, ...] (the changed
 *  levels), else ["ap","bus", ...] (every tag). Whole entries only, up to @p cap (with the
 *  NUL); a tag is never escaped, so the table must hold only tags that gw_logtag_valid()
 *  accepts or that a log line gave (gw_logring_tag stops at a space or ':'). Returns the
 *  length. */
size_t gw_logtags_json(const gw_logtags_t *t, bool changed_only, char *out, size_t cap);

/** A tag that a request may set: 1 to GW_LOGTAG_SIZE - 1 printable ASCII characters without
 *  spaces, ':', '"' and '\\', or "*". */
bool gw_logtag_valid(const char *tag);

/**
 * #199: the body of POST /api/log/level: {"tag": "poll", "level": "debug"} or {"reset": true}.
 * @p tag gets the tag (GW_LOGTAG_SIZE), @p level the level; @p reset is true for a reset (then
 * the other two are not read). @p max_level: the highest level of the build. False with the
 * reason in @p why. Takes the JSON lock (gw_port_json_lock).
 */
bool gw_logring_parse_set(const char *json, size_t len, int max_level, char *tag, int *level,
                          bool *reset, char *why, size_t cap);

#ifdef __cplusplus
}
#endif
