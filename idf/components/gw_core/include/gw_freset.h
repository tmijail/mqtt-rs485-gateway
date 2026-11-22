/*
 * CFG-5 (issue #266): the sequence of the factory reset, pure C (host-tested).
 *
 * The factory reset erases two stores, LittleFS and the NVS, and neither erase is atomic. The
 * factory marker (commissioning mode at the next boot) is in the NVS, so no single marker
 * survives both erases. Two markers hand the state "a reset is pending" from one store to the
 * other, so that it is durable at each moment:
 *
 *   step                 what it does                                  pending is held by
 *   GW_FRESET_NVS_OPEN   only when the NVS is not usable: erase it     (nothing yet)
 *                        and start it, so that it can hold the intent
 *   GW_FRESET_INTENT     write the intent to the NVS                   the intent
 *   GW_FRESET_WIPE_FS    erase LittleFS fully, then format it          the intent
 *   GW_FRESET_FS_MARK    write the marker file to LittleFS             the intent and the file
 *   GW_FRESET_ERASE_NVS  erase the NVS fully, then start it            the file
 *   GW_FRESET_FACTORY    write the factory marker to the NVS           the file
 *   GW_FRESET_FS_UNMARK  delete the marker file                        (the reset is complete)
 *
 * A step that fails stops the sequence: the markers stay, and the next boot does the remaining
 * steps again. Each step is idempotent.
 *
 * At boot (gw_freset_resume), before the mode is selected:
 *   the intent is in the NVS             -> WIPE_FS, FS_MARK, ERASE_NVS, FACTORY, FS_UNMARK
 *   else the marker file is in LittleFS  -> ERASE_NVS, FACTORY, FS_UNMARK
 *   else                                 -> nothing is pending
 * The intent is examined first. While the NVS holds it, the NVS erase did not complete, and a
 * second wipe of LittleFS loses nothing. A marker file with the intent can also be the file of
 * an earlier reset that did not complete, in a LittleFS that got new files after it. With the
 * marker file alone, the NVS erase is always done again: the NVS can be partly erased, and a
 * factory marker of an earlier reset can be there with secrets of a later time.
 *
 * The boot then selects the source of the configuration with gw_freset_boot(). A reset that is
 * still pending (a step failed again, or safe mode did not do the steps) gives recovery mode:
 * the embedded example configuration is used only when no reset is pending, no factory marker
 * is set, LittleFS mounts and it has no configuration file. This is the state of a board after
 * its first flash, which stays as it was.
 *
 * What a power cut gives at the next boot:
 *   before INTENT is durable         the state before the reset (nothing was erased)
 *   from INTENT until the NVS erase  the boot wipes LittleFS again and completes the reset
 *     removes the intent
 *   from then to FS_UNMARK           the boot erases the NVS again and completes the reset
 * One exception, only for a reset that starts with an NVS that is not usable (or that cannot
 * hold the intent): a cut during GW_FRESET_NVS_OPEN leaves the configuration file, and all,
 * part or none of an NVS that the gateway could not use before. It is not a state that the
 * reset made worse, but it is not exactly the state before the reset.
 * If the NVS cannot hold the intent also after GW_FRESET_NVS_OPEN (a hardware fault), the reset
 * goes on without it: the owner must be able to erase the configuration. Then only the short
 * time between the format of LittleFS and the marker file has no marker.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_FRESET_NVS_OPEN = 0,
    GW_FRESET_INTENT,
    GW_FRESET_WIPE_FS,
    GW_FRESET_FS_MARK,
    GW_FRESET_ERASE_NVS,
    GW_FRESET_FACTORY,
    GW_FRESET_FS_UNMARK,
    GW_FRESET_STEPS
} gw_freset_step_t;

/* The storage as the sequence sees it (gw_storage on the board, a model in the host test). */
typedef struct {
    void *ctx;
    /* the NVS is started and can be read and written */
    bool (*nvs_usable)(void *ctx);
    /* the NVS holds the intent; false also when the NVS is not usable */
    bool (*intent)(void *ctx);
    /* LittleFS mounts and holds the marker file */
    bool (*fs_mark)(void *ctx);
    /* does one step; true: the step is done and its result is durable */
    bool (*step)(void *ctx, gw_freset_step_t step);
} gw_freset_ops_t;

typedef enum {
    GW_FRESET_IDLE = 0,    /* no reset is pending */
    GW_FRESET_FROM_WIPE,   /* the intent: all the steps from the wipe of LittleFS */
    GW_FRESET_FROM_NVS,    /* the marker file alone: all the steps from the erase of the NVS */
} gw_freset_pending_t;

typedef enum {
    GW_FRESET_NONE = 0,    /* gw_freset_resume: nothing was pending, nothing was done */
    GW_FRESET_DONE,        /* all the steps are done: the next boot is commissioning mode */
    GW_FRESET_FAILED,      /* a step failed: the reset stays pending (or, when the intent could
                            * not be written and nothing else was done, it did not start) */
} gw_freset_result_t;

/** What the stores tell about a reset that did not complete. */
gw_freset_pending_t gw_freset_pending(const gw_freset_ops_t *ops);

/** The full sequence, for a request of the owner. @p failed (can be NULL): the step that failed,
 *  with GW_FRESET_FAILED. */
gw_freset_result_t gw_freset_run(const gw_freset_ops_t *ops, gw_freset_step_t *failed);

/** At boot: the remaining steps of a pending reset. @p failed as gw_freset_run(). */
gw_freset_result_t gw_freset_resume(const gw_freset_ops_t *ops, gw_freset_step_t *failed);

/** A short name of @p step for the log ("?" for a value that is not a step). */
const char *gw_freset_step_name(gw_freset_step_t step);

/* Where the boot takes the configuration from. */
typedef enum {
    GW_FRESET_BOOT_PENDING = 0,    /* a reset is pending: recovery mode, no configuration is read */
    GW_FRESET_BOOT_COMMISSIONING,  /* the factory marker and no configuration file */
    GW_FRESET_BOOT_NO_FS,          /* LittleFS does not mount: recovery mode */
    GW_FRESET_BOOT_FILE,           /* the configuration file (recovery mode if it is rejected) */
    GW_FRESET_BOOT_EXAMPLE,        /* the embedded example configuration */
} gw_freset_boot_t;

/** @p pending: gw_freset_pending() is not GW_FRESET_IDLE. @p factory: the factory marker is in
 *  the NVS. @p fs: LittleFS mounts. @p file: it has a configuration file (only with @p fs).
 *  With GW_FRESET_BOOT_FILE and @p factory, a configuration was flashed after the reset: the
 *  caller removes the factory marker. */
gw_freset_boot_t gw_freset_boot(bool pending, bool factory, bool fs, bool file);

/**
 * Issue #280: a board whose LittleFS partition is blank: a new board, or a flash with a full
 * erase that wrote no LittleFS image (the install tool of #219). True: the boot writes the
 * factory marker to the NVS, and only then formats the partition, so that gw_freset_boot() gives
 * GW_FRESET_BOOT_COMMISSIONING and never GW_FRESET_BOOT_EXAMPLE.
 * @p pending: a factory reset is pending (its own steps format the partition). @p nvs_usable:
 * the NVS can hold the factory marker (without it a format would give the example
 * configuration). @p fs: LittleFS mounts. @p blank: the superblocks of the partition are erased
 * (all 0xFF). A partition that does not mount and is not blank is never formatted here: it can
 * hold a configuration, or the copy of an NVS migration (#104, #109).
 */
bool gw_freset_adopt_blank(bool pending, bool nvs_usable, bool fs, bool blank);

#ifdef __cplusplus
}
#endif
