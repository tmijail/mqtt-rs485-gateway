/*
 * FW-1 (issue #144 item 5): the result of the last update, pure C (host-tested).
 *
 * One NVS string (key "fw_last", at most GW_FWLAST_MAX bytes with the NUL), never a raw struct,
 * so that every image reads it: "r=<result>;m=<expected mode>;v=<version>;c=<cause>". Unknown
 * keys are ignored; the cause is the rest of the string. The firmware writes it before each step
 * that can end in a restart (the boot partition of an update, the rejection of a new image), and
 * with each other result. Facts that cross images come from this record, the reset reason and
 * otadata; RTC memory keeps only the boot-loop counter of the running image.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_ota.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_FWLAST_MAX 96
#define GW_FWLAST_VERSION 32   /* esp_app_desc_t.version, with the NUL */

typedef enum {
    GW_FWLAST_NONE = 0,      /* no record: "idle" */
    GW_FWLAST_RESTARTING,    /* r=s: the update is set to boot, the restart follows */
    GW_FWLAST_TESTING,       /* r=t: the new image runs on trial */
    GW_FWLAST_INSTALLED,     /* r=i: the new image is confirmed */
    GW_FWLAST_FAILED,        /* r=f: the update did not take effect (the cause says why) */
    GW_FWLAST_REJECTED,      /* r=j: the new image rejected itself; its restart follows */
    GW_FWLAST_ROLLED_BACK,   /* r=b: the earlier image runs again */
} gw_fwlast_result_t;

typedef struct {
    gw_fwlast_result_t result;
    gw_ota_mode_t expect;    /* the mode that the new image must reach (NORMAL or RECOVERY) */
    char version[GW_FWLAST_VERSION];   /* of the new image ("" when unknown) */
    char cause[GW_FWLAST_MAX];
} gw_fwlast_t;

/** The record as text (at most GW_FWLAST_MAX - 1 characters: the cause is cut to fit, at a
 *  character boundary; a ';' in the version becomes '_'). Returns its length. */
size_t gw_fwlast_format(const gw_fwlast_t *r, char *out, size_t cap);

/** Parse @p s; false (and *r cleared) when it holds no known result. */
bool gw_fwlast_parse(const char *s, gw_fwlast_t *r);

/* What the boot knows besides the record. */
typedef struct {
    bool pending;            /* the running image is on trial */
    const char *running;     /* its version */
    bool other_rejected;     /* otadata: the other slot is ABORTED or INVALID */
    const char *reset;       /* the reset reason ("panic", "software", ...) */
} gw_fwlast_boot_t;

/** The record at the boot. For a pending image, @p expected gets the mode stored with its update
 *  (RECOVERY when the record is not about this image: an installer without the record). Returns
 *  true when the record changed and must be written:
 *  - a pending image: TESTING;
 *  - the earlier image after a restart during the test (RESTARTING or TESTING of another
 *    version, the other slot rejected): ROLLED_BACK, "restarted during the test (<reset>)";
 *  - the same without a rejected other slot (the update never booted): FAILED, "interrupted
 *    before the restart (<reset>)";
 *  - the earlier image after a rejection (REJECTED of another version): ROLLED_BACK;
 *  - the image of the record, not pending (its confirmation was not recorded): INSTALLED. */
bool gw_fwlast_on_boot(gw_fwlast_t *r, const gw_fwlast_boot_t *b, gw_ota_mode_t *expected);

/** "Firmware Update/State" of the record (issue #144 item 7): "idle", "restarting into <v>",
 *  "testing <v>", "installed <v>", "failed: <cause>", "rolled back from <v>: <cause>". An
 *  INSTALLED record of another version than @p running (a serial flash since) is "idle". */
void gw_fwlast_state(const gw_fwlast_t *r, const char *running, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
