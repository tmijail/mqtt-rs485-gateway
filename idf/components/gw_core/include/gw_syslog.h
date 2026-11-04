/*
 * Remote syslog (REL-4): the pure part. Parses esp_log lines, formats RFC 5424
 * messages and keeps a bounded single-producer / single-consumer ring of
 * pending records. No OS calls; the IDF glue (gw_log) serialises producers,
 * owns the UDP socket and decides when to send.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SYSLOG_TAG_MAX  33    /* APP-NAME (RFC 5424 allows 48) */
#define GW_SYSLOG_MSG_MAX  200   /* longer messages are truncated */
#define GW_SYSLOG_FACILITY 16    /* local0 */

typedef struct {
    uint64_t time_ms;            /* UTC; 0 = clock not set (NILVALUE timestamp) */
    uint8_t severity;            /* RFC 5424: 3 error, 4 warning, 6 info, 7 debug */
    char tag[GW_SYSLOG_TAG_MAX];
    char msg[GW_SYSLOG_MSG_MAX];
} gw_syslog_rec_t;

/** Severity of an esp_log format string ("I (%lu) %s: ..."), from its first letter; -1 if
 * the string does not start with a level letter. Lets the caller filter before formatting. */
int gw_syslog_format_severity(const char *fmt);

/**
 * Split one formatted esp_log line ("W (1234) edge: text\n", optionally with ANSI colours
 * and a "(hh:mm:ss.mmm)" timestamp) into severity, tag and message. Lines that do not have
 * that shape are kept whole as an info message with tag "-"; returns false for them.
 * time_ms is left untouched.
 */
bool gw_syslog_parse_line(const char *line, gw_syslog_rec_t *rec);

/**
 * RFC 5424 message: "<PRI>1 TIMESTAMP HOSTNAME APP-NAME - - [meta sequenceId="N"] MSG".
 * HOSTNAME and APP-NAME are reduced to printable US-ASCII without spaces; control
 * characters in MSG become spaces. Returns the length written (truncated to cap - 1, NUL
 * terminated).
 */
size_t gw_syslog_format(const gw_syslog_rec_t *rec, const char *hostname, uint32_t sequence,
                        char *out, size_t cap);

/** "2026-11-04T12:34:56.789Z" (24 characters + NUL). */
void gw_syslog_timestamp(uint64_t time_ms, char out[25]);

/* ---- bounded ring: one producer (callers serialised by the glue), one consumer ---- */

typedef struct {
    gw_syslog_rec_t *slots;
    unsigned count;              /* capacity is count - 1 */
    atomic_uint head;            /* next slot the producer writes */
    atomic_uint tail;            /* next slot the consumer reads */
    atomic_uint dropped;         /* records lost because the ring was full */
} gw_syslog_ring_t;

void gw_syslog_ring_init(gw_syslog_ring_t *r, gw_syslog_rec_t *slots, unsigned count);

/** Slot to fill, or NULL when full (the record is counted as dropped). */
gw_syslog_rec_t *gw_syslog_ring_reserve(gw_syslog_ring_t *r);

/** Publish the slot returned by the last reserve. */
void gw_syslog_ring_commit(gw_syslog_ring_t *r);

/** Oldest pending record, or NULL. */
const gw_syslog_rec_t *gw_syslog_ring_peek(gw_syslog_ring_t *r);

/** Release the record returned by peek. */
void gw_syslog_ring_pop(gw_syslog_ring_t *r);

/** Records dropped since the last call (and reset the counter). */
unsigned gw_syslog_ring_take_dropped(gw_syslog_ring_t *r);

/** Count a record lost before reaching the ring (e.g. producer contention). */
void gw_syslog_ring_count_drop(gw_syslog_ring_t *r);

#ifdef __cplusplus
}
#endif
