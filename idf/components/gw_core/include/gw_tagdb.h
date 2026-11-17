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

/** One tag's value. The member in use follows the tag's configuration (gw_conv_kind). */
typedef struct {
    union {
        double value;              /* every type up to 32 bits, float32/float64, scaled tags */
        int64_t i64;               /* unscaled int64: exact (a double is not above 2^53) */
        uint64_t u64;              /* unscaled uint64 */
        char str[GW_STR_MAX + 1];  /* string, NUL terminated */
    };
    uint64_t ts_ms;      /* acquisition time, UTC ms */
    bool valid;          /* false: never read, read failed with an exception, or device offline */
} gw_tag_value_t;

/** Per-device diagnostics (MB-13), kept by the poll engine. */
typedef struct {
    uint32_t cycles;          /* poll cycles run */
    uint32_t failed_cycles;   /* cycles with a request unanswered after its retries */
    mb_stats_t bus;           /* outcome of every attempt (retries included) to this device */
    mb_stats_t tcp;           /* MB-15: attempts of Modbus TCP requests forwarded to its unit */
} gw_dev_stats_t;

typedef struct {
    uint8_t di;          /* bit n = DI n+1 (debounced, true = active) */
    uint8_t dout;        /* bit n = DO n+1 (read back from the expander) */
    bool do_unknown;     /* the expander does not answer: DO states unknown (published as null) */
    bool failsafe;       /* IO-3: the DOs took their safe state (no Sparkplug session) */
    uint64_t di_ts_ms;
    uint64_t do_ts_ms;
    uint64_t count[8];   /* IO-4: activations (inactive -> active) of DI n+1; #218: the PCNT
                          * edges of a fast DI */
    uint64_t count_ts_ms;   /* last change of a counter */
    uint8_t do_rise;     /* #210: DOs switched on since gw_tagdb_take_do_rise() */
    uint64_t do_rise_ts_ms;
    uint64_t run_s[GW_RUN_HOURS_CHANNELS];   /* #217: run hours (s) of DI/1..8, DO/1..8 */
    uint64_t run_ts_ms;
} gw_local_io_t;

/** Clear everything (all devices offline, all values invalid). */
void gw_tagdb_reset(void);

/** Commit a device's values and online flag. @p vals has the device's tag_count entries. */
void gw_tagdb_put_device(const gw_config_t *cfg, uint16_t dev, const gw_tag_value_t *vals,
                         bool online);

/** Copy a device's values (tag_count entries) and online flag. */
void gw_tagdb_get_device(const gw_config_t *cfg, uint16_t dev, gw_tag_value_t *vals, bool *online);

void gw_tagdb_put_stats(uint16_t dev, const gw_dev_stats_t *stats);
/** Online flag of @p dev, without copying its values. */
bool gw_tagdb_device_online(uint16_t dev);
void gw_tagdb_get_stats(uint16_t dev, gw_dev_stats_t *stats);

/** Update one tag (write confirmation). */
void gw_tagdb_put_tag(uint16_t tag, const gw_tag_value_t *val);
void gw_tagdb_get_tag(uint16_t tag, gw_tag_value_t *val);

/** DI states without counting (start-up). */
void gw_tagdb_set_di(uint8_t di, uint64_t ts_ms);
/** New debounced DI states: every input that became active counts one pulse (IO-4), except
 *  the fast DIs (#218: the PCNT counts them, gw_tagdb_add_count()). */
void gw_tagdb_update_di(uint8_t di, uint64_t ts_ms);
/** #218: the DIs that the PCNT counts. */
void gw_tagdb_set_fast(uint8_t mask);
/** #218: @p n more pulses of DI @p index (0-7) from its PCNT unit. */
void gw_tagdb_add_count(uint8_t index, uint32_t n, uint64_t ts_ms);
/** #217: the run hours to publish (s; GW_RUN_HOURS_CHANNELS values). */
void gw_tagdb_set_run_hours(const uint64_t run_s[GW_RUN_HOURS_CHANNELS], uint64_t ts_ms);
/** #210: the DOs switched on since the last call (and the time of the last one), for the
 *  Sparkplug task only: a pulse that ended before the NDATA still gives true, then false. */
uint8_t gw_tagdb_take_do_rise(uint64_t *ts_ms);
/** IO-4: counters restored at boot. */
void gw_tagdb_set_counters(const uint64_t count[8]);
void gw_tagdb_set_do(uint8_t dout, uint64_t ts_ms);   /* also clears do_unknown */
void gw_tagdb_set_do_unknown(uint64_t ts_ms);
void gw_tagdb_set_failsafe(bool on);
void gw_tagdb_get_io(gw_local_io_t *io);

#ifdef __cplusplus
}
#endif
