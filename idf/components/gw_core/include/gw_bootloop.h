/*
 * FW-1 (issue #144 item 4): the boot-loop counter, pure C (host-tested).
 *
 * The record is in RTC memory that a reset does not clear (RTC_NOINIT, gw_fw), with a magic
 * number and a CRC. The firmware updates it early at each boot, before gw_storage_init(), from
 * the reset reason:
 *   +1          panic, interrupt watchdog, task watchdog, other watchdog resets, CPU lock-up;
 *   set to 0    power-on, brownout, power glitch, a record with a bad magic number or CRC;
 *   no change   a planned restart (software), the reset button, the other reasons.
 * At GW_BOOTLOOP_SAFE (3) the gateway boots in safe mode (recovery mode, cause "repeated
 * crashes"). A pending image (on trial) ignores the counter: its trial protects it. The counter
 * is cleared after 120 s of uptime in normal mode and on an operator action; safe mode ends after
 * 60 min with the counter at GW_BOOTLOOP_SAFE - 1 (one more try of normal mode).
 * The address of the record can change between images: an image that finds a record of another
 * image sees a bad CRC and starts at 0.
 *
 * Issue #209: an operator can also ask for safe mode (POST /api/restart?safe=1, the shell's
 * 'restart safe'). The request is a field of the same record: the next boot takes it (one boot
 * only), and it does not change the count. Safe mode on request also ends after 60 min or at the
 * next restart; then the count stays as it was. A power-on, a brownout or a power glitch removes
 * a request that a boot did not take. Before v1.0 the record of an earlier image is dropped (its
 * size changed: the CRC does not agree), and the count starts at 0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_BOOTLOOP_MAGIC 0x504C4247u   /* "GBLP" */
#define GW_BOOTLOOP_SAFE 3u
#define GW_BOOTLOOP_REQUEST 0x45464153u   /* "SAFE": issue #209, the next boot starts safe mode */

/* The reset reasons that the counter tells apart (esp_reset_reason(), gw_fw maps them). */
typedef enum {
    GW_RESET_OTHER = 0,      /* deep sleep, USB, JTAG, SDIO, eFuse, unknown: no change */
    GW_RESET_POWERON,
    GW_RESET_BROWNOUT,
    GW_RESET_PWR_GLITCH,
    GW_RESET_SOFTWARE,       /* esp_restart(): a planned restart */
    GW_RESET_EXTERNAL,       /* the reset button */
    GW_RESET_PANIC,
    GW_RESET_INT_WDT,
    GW_RESET_TASK_WDT,
    GW_RESET_WDT,            /* other watchdogs (RTC, MWDT) */
    GW_RESET_CPU_LOCKUP,
} gw_reset_kind_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t request;        /* GW_BOOTLOOP_REQUEST: safe mode at the next boot (issue #209) */
    uint32_t crc;
} gw_bootloop_t;

/** The boot: updates @p r from @p why and returns the count. A @p pending image leaves the count
 *  as it is (only a bad record is set to 0). */
uint32_t gw_bootloop_boot(gw_bootloop_t *r, gw_reset_kind_t why, bool pending);

/** The count of a valid record, 0 otherwise. */
uint32_t gw_bootloop_count(const gw_bootloop_t *r);

/** Sets the count (0: cleared; GW_BOOTLOOP_SAFE - 1: the retry after safe mode). A request of a
 *  valid record stays. */
void gw_bootloop_set(gw_bootloop_t *r, uint32_t count);

/** Issue #209: the next boot starts safe mode (the count does not change). */
void gw_bootloop_request(gw_bootloop_t *r);

/** Issue #209: true when the record holds a request; the request is removed (one boot only). Call
 *  it after gw_bootloop_boot(). */
bool gw_bootloop_take_request(gw_bootloop_t *r);

/** True when a boot with @p count starts safe mode (a pending image never does). */
bool gw_bootloop_safe(uint32_t count, bool pending);

#ifdef __cplusplus
}
#endif
