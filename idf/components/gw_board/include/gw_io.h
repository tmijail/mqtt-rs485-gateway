/* Local I/O: 8 debounced digital inputs (IO-1) and 8 digital outputs on the
 * TCA9554 I2C expander (IO-2) with their safe state (IO-3), the DI pulse counters (IO-4)
 * and the local DI -> DO rules (IO-7). Pins and active levels come
 * from gw_bsp; the states published through gw_tagdb are logical (1 = input asserted /
 * output on).
 *
 * Single owner of the expander: before the configuration is loaded, app_main sets the
 * outputs up (gw_io_outputs_boot, gw_io_outputs_all_off); from gw_io_start() on, only the
 * local_io task writes them: DO commands (gw_io_set_do, from the Sparkplug task), the local
 * rules (which own their DOs: NCMD on them is refused), the safe state and the retries of
 * an expander that does not answer. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * First step of a restart, before the configuration is loaded: every DO off, except the
 * outputs of @p hold (IO-3 "hold" of the last valid configuration, kept in NVS) whose pin
 * the expander still drives from before a warm reset: they keep their state. The latch is
 * loaded before the pins become outputs, so there is no glitch (see gw_dout.h).
 * Returns the error if the expander does not answer: the DOs are then unknown (null in
 * gw_tagdb), DO commands are refused and the local_io task retries with a backoff.
 */
esp_err_t gw_io_outputs_boot(uint8_t hold);

/** No valid configuration (recovery, commissioning): held outputs are switched off too. */
void gw_io_outputs_all_off(void);

/**
 * Issue #74: without a valid configuration, start the local_io task anyway, for the only work
 * left: retrying an expander that did not answer at boot, with every DO off as the target (the
 * latch may still hold outputs on from before a warm reset), and the RTC writes. No DI
 * sampling, rules, failsafe or DO commands.
 */
esp_err_t gw_io_start_recovery(void);

/** State of the DO expander, for the shell and /api/status. */
typedef struct {
    bool ready;            /* answered: the DOs are known */
    bool retrying;         /* not answering; the local_io task retries with a backoff */
    uint16_t attempts;     /* initialisation attempts so far */
} gw_io_expander_t;
void gw_io_get_expander(gw_io_expander_t *out);

/**
 * With a valid configuration: put the DOs in their restart state (do_failsafe "on" / "off",
 * "hold" kept), start the local_io task and the DI sampling every CONFIG_GW_DI_SAMPLE_US
 * with the configured debounce. @p on_change is called whenever a debounced DI or a DO
 * changed (from the esp_timer task or the local_io task).
 */
esp_err_t gw_io_start(const gw_config_t *cfg, void (*on_change)(void));

/** Switch DO @p index (0-7) on or off through the local_io task and wait for it (at most
 *  500 ms). The read-back state is in gw_tagdb. Single caller (the Sparkplug task).
 *  Returns false (and changes nothing) if the expander is not available or a local rule
 *  drives that DO (IO-7). */
bool gw_io_set_do(uint8_t index, bool on);

/* ---- TIME-2: PCF85063 RTC on the same I2C bus (same single owner) ---- */
typedef struct {
    bool present;          /* the chip answered */
    bool restored;         /* the system time came from it at boot */
    int status;            /* gw_rtc_status_t of the boot reading (or of the last store) */
    uint32_t stores;       /* times written after SNTP or a browser time (#160) */
    uint32_t store_errors;
} gw_io_rtc_info_t;

/** At boot, after gw_io_outputs_boot() (the I2C bus) and before anything uses the time: set
 *  the system time from the RTC if its oscillator ran (OS bit clear) and the time is
 *  plausible. gw_io_rtc_get() tells whether it did. */
void gw_io_rtc_restore(void);

/** After each SNTP synchronisation, and after a time set from the browser (issue #160), from
 *  any task: store the system time in the RTC. Done by the
 *  local_io task once it runs, the only user of the bus. */
void gw_io_rtc_store(void);

void gw_io_rtc_get(gw_io_rtc_info_t *out);

#if CONFIG_GW_IO_SIMULATED
/** Emulator only: set the logical level of DI @p index (0-7); used by the console shell. */
void gw_io_sim_set_di(uint8_t index, bool active);
/** Emulator only (issue #74): the expander does not answer during the first @p ms of the next
 *  boot. */
void gw_io_sim_expander_absent(uint32_t ms);
#endif

#ifdef __cplusplus
}
#endif
