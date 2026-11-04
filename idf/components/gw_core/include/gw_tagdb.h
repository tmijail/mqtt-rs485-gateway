/*
 * Shared process image: the only state shared between the RS485 owner task
 * (writer of tag values and device state), the local I/O layer (writer of DI/DO)
 * and the Sparkplug task (reader). Every access copies under gw_port_lock(), so
 * critical sections are short and bounded. Statically allocated (REL-2).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gw_config.h"
#include "mb_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double value;
    uint64_t ts_ms;      /* acquisition time, UTC ms */
    bool valid;          /* false: never read, read failed with an exception, or device offline */
} gw_tag_value_t;

/** Per-device diagnostics (MB-13), kept by the poll engine. */
typedef struct {
    uint32_t cycles;          /* poll cycles run */
    uint32_t failed_cycles;   /* cycles with a request unanswered after its retries */
    mb_stats_t bus;           /* outcome of every attempt (retries included) to this device */
} gw_dev_stats_t;

typedef struct {
    uint8_t di;          /* bit n = DI n+1 (debounced, true = active) */
    uint8_t dout;        /* bit n = DO n+1 (read back from the expander) */
    bool do_unknown;     /* the expander does not answer: DO states unknown (published as null) */
    uint64_t di_ts_ms;
    uint64_t do_ts_ms;
} gw_local_io_t;

/** Clear everything (all devices offline, all values invalid). */
void gw_tagdb_reset(void);

/** Commit a device's values and online flag. @p vals has the device's tag_count entries. */
void gw_tagdb_put_device(const gw_config_t *cfg, uint16_t dev, const gw_tag_value_t *vals,
                         bool online);

/** Copy a device's values (tag_count entries) and online flag. */
void gw_tagdb_get_device(const gw_config_t *cfg, uint16_t dev, gw_tag_value_t *vals, bool *online);

void gw_tagdb_put_stats(uint16_t dev, const gw_dev_stats_t *stats);
void gw_tagdb_get_stats(uint16_t dev, gw_dev_stats_t *stats);

/** Update one tag (write confirmation). */
void gw_tagdb_put_tag(uint16_t tag, const gw_tag_value_t *val);
void gw_tagdb_get_tag(uint16_t tag, gw_tag_value_t *val);

void gw_tagdb_set_di(uint8_t di, uint64_t ts_ms);
void gw_tagdb_set_do(uint8_t dout, uint64_t ts_ms);   /* also clears do_unknown */
void gw_tagdb_set_do_unknown(uint64_t ts_ms);
void gw_tagdb_get_io(gw_local_io_t *io);

#ifdef __cplusplus
}
#endif
