/*
 * SPB-14 / issues #139 and #137: the state of the microSD card and of its format, for the
 * shell, /api/status and the web UI.
 *
 * The format runs in a task of its own (gw_sfcard); the shell and the web API only start it.
 * This module holds what is pure logic: whether a request may start a format (one at a time,
 * a build with a card, a board with PSRAM), the phases of the format (idle, running, done,
 * failed) and their text and JSON. No OS calls, no allocation; the caller serializes access
 * (gw_sfcard keeps both structures behind a spinlock and copies them out).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the card ---- */

typedef enum {
    GW_CARD_UNSUPPORTED = 0, /* the build has no microSD card (QEMU stand-in, or none) */
    GW_CARD_UNCHECKED,       /* not probed: the spill is off, or it did not start before */
    GW_CARD_ABSENT,          /* no card answered */
    GW_CARD_UNMOUNTED,       /* a card answered, but it has no FAT file system it can use */
    GW_CARD_MOUNTED,
    GW_CARD_FORMATTING,
    GW_CARD_FORMATTED,       /* formatted since the boot: used after a restart */
    GW_CARD_STATE_COUNT
} gw_card_state_t;

#define GW_CARD_UNKNOWN UINT32_MAX   /* a size or count that is not known */

typedef struct {
    gw_card_state_t state;
    char detail[80];         /* why (unsupported, unchecked, absent, not mounted), or "" */
    char name[8];            /* the card's product name (CID), "" = unknown */
    char fat[6];             /* "FAT12", "FAT16", "FAT32", "" = unknown */
    uint32_t cluster;        /* bytes, 0 = unknown */
    uint32_t size_mb;        /* capacity of the card, 0 = unknown */
    uint32_t free_mb;        /* free space (FSInfo, no FAT scan), GW_CARD_UNKNOWN = unknown */
    uint32_t other;          /* entries of the root other than "sf", GW_CARD_UNKNOWN = unknown */
} gw_card_info_t;

/** "unsupported", "unchecked", "absent", "not_mounted", "mounted", "formatting",
 *  "formatted". */
const char *gw_card_state_name(gw_card_state_t s);

/** One line for the shell (no newline). Returns the length written (truncated to @p cap). */
size_t gw_card_text(const gw_card_info_t *c, char *out, size_t cap);

/** A JSON object {"state":..., "detail":..., ...}; the fields that are not known are left out.
 *  Returns the length, or 0 when it does not fit in @p cap (then @p out is "{}"). */
size_t gw_card_json(const gw_card_info_t *c, char *out, size_t cap);

/* ---- the format ---- */

typedef enum {
    GW_CARDFMT_IDLE = 0,     /* no format since the boot */
    GW_CARDFMT_RUNNING,
    GW_CARDFMT_DONE,
    GW_CARDFMT_FAILED,
} gw_cardfmt_phase_t;

/** The answer to a request. */
typedef enum {
    GW_CARDFMT_STARTED = 0,
    GW_CARDFMT_BUSY,         /* a format runs */
    GW_CARDFMT_NO_SUPPORT,   /* the build has no microSD card */
    GW_CARDFMT_NO_PSRAM,     /* the format, as the spill, needs the PSRAM */
    GW_CARDFMT_NO_MEMORY,    /* its task could not be created */
} gw_cardfmt_answer_t;

typedef struct {
    gw_cardfmt_phase_t phase;
    bool simulated;          /* emulator test run: nothing written */
    uint32_t start_ms;       /* the clock of the caller (ms, may wrap) */
    uint32_t took_ms;        /* done or failed: the duration */
    uint32_t count;          /* formats started since the boot */
    char fat[6];             /* done: "FAT16" or "FAT32" */
    uint32_t cluster;        /* done: bytes */
    uint32_t size_mb;        /* done: size of the new volume */
    char why[96];            /* failed: the reason */
} gw_cardfmt_t;

/**
 * A request to format. A format that runs refuses the next one (BUSY), before anything else.
 * Then the build needs a card (@p support) and the board the PSRAM (@p psram); an emulator
 * test run (@p simulate) needs neither. On STARTED the state is RUNNING from @p now_ms; on any
 * other answer it does not change.
 */
gw_cardfmt_answer_t gw_cardfmt_request(gw_cardfmt_t *st, bool support, bool psram,
                                       bool simulate, uint32_t now_ms);

/** The format ended well: FAT type, cluster size (bytes) and size of the volume (MB). */
void gw_cardfmt_done(gw_cardfmt_t *st, uint32_t now_ms, const char *fat, uint32_t cluster,
                     uint32_t size_mb);

/** The format ended with an error (or did not start: no card); @p why says why. */
void gw_cardfmt_failed(gw_cardfmt_t *st, uint32_t now_ms, const char *why);

/** "idle", "running", "done", "failed". */
const char *gw_cardfmt_phase_name(gw_cardfmt_phase_t p);

/** "started", "busy", "no_support", "no_psram", "no_memory" (the API's "reason"). */
const char *gw_cardfmt_answer_key(gw_cardfmt_answer_t a);

/** The HTTP status line of the API: 202, 409, 501 (no card in the build, no PSRAM), 503. */
const char *gw_cardfmt_answer_http(gw_cardfmt_answer_t a);

/** The answer for the shell, in English. */
const char *gw_cardfmt_answer_text(gw_cardfmt_answer_t a);

/** Milliseconds since the start (running) or the duration (done, failed). */
uint32_t gw_cardfmt_elapsed_ms(const gw_cardfmt_t *st, uint32_t now_ms);

/** One line for the shell (no newline). Returns the length written (truncated to @p cap). */
size_t gw_cardfmt_text(const gw_cardfmt_t *st, uint32_t now_ms, char *out, size_t cap);

/** A JSON object {"state":..., ...} for /api/status. Returns the length, or 0 when it does not
 *  fit (then @p out is "{}"). */
size_t gw_cardfmt_json(const gw_cardfmt_t *st, uint32_t now_ms, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
