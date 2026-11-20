#include "gw_freset.h"

#include <stddef.h>

gw_freset_pending_t gw_freset_pending(const gw_freset_ops_t *ops)
{
    /* the intent first: a marker file with it can be the file of an earlier reset (gw_freset.h) */
    if (ops->intent(ops->ctx)) {
        return GW_FRESET_FROM_WIPE;
    }
    return ops->fs_mark(ops->ctx) ? GW_FRESET_FROM_NVS : GW_FRESET_IDLE;
}

/* The steps @p from to GW_FRESET_FS_UNMARK, until one fails. */
static gw_freset_result_t steps_from(const gw_freset_ops_t *ops, gw_freset_step_t from,
                                     gw_freset_step_t *failed)
{
    for (int s = (int)from; s < (int)GW_FRESET_STEPS; s++) {
        if (!ops->step(ops->ctx, (gw_freset_step_t)s)) {
            if (failed != NULL) {
                *failed = (gw_freset_step_t)s;
            }
            return GW_FRESET_FAILED;
        }
    }
    return GW_FRESET_DONE;
}

gw_freset_result_t gw_freset_run(const gw_freset_ops_t *ops, gw_freset_step_t *failed)
{
    /* The intent must be durable before LittleFS loses the configuration. An NVS that is not
     * usable, or that refuses the intent (for example it is full), is erased first: the reset
     * erases it in any case. */
    bool intent = ops->nvs_usable(ops->ctx) && ops->step(ops->ctx, GW_FRESET_INTENT);
    if (!intent && ops->step(ops->ctx, GW_FRESET_NVS_OPEN)) {
        /* without the intent (the NVS has a hardware fault) the erase goes on: gw_freset.h */
        (void)ops->step(ops->ctx, GW_FRESET_INTENT);
    }
    return steps_from(ops, GW_FRESET_WIPE_FS, failed);
}

gw_freset_result_t gw_freset_resume(const gw_freset_ops_t *ops, gw_freset_step_t *failed)
{
    switch (gw_freset_pending(ops)) {
    case GW_FRESET_FROM_NVS:
        return steps_from(ops, GW_FRESET_ERASE_NVS, failed);
    case GW_FRESET_FROM_WIPE:
        return steps_from(ops, GW_FRESET_WIPE_FS, failed);
    case GW_FRESET_IDLE:
    default:
        return GW_FRESET_NONE;
    }
}

const char *gw_freset_step_name(gw_freset_step_t step)
{
    switch (step) {
    case GW_FRESET_NVS_OPEN: return "erase of an NVS that is not usable";
    case GW_FRESET_INTENT: return "intent in the NVS";
    case GW_FRESET_WIPE_FS: return "erase of LittleFS";
    case GW_FRESET_FS_MARK: return "marker file in LittleFS";
    case GW_FRESET_ERASE_NVS: return "erase of the NVS";
    case GW_FRESET_FACTORY: return "factory marker in the NVS";
    case GW_FRESET_FS_UNMARK: return "removal of the marker file";
    case GW_FRESET_STEPS:
    default: return "?";
    }
}

gw_freset_boot_t gw_freset_boot(bool pending, bool factory, bool fs, bool file)
{
    if (pending) {
        return GW_FRESET_BOOT_PENDING;
    }
    if (factory && !(fs && file)) {
        return GW_FRESET_BOOT_COMMISSIONING;
    }
    if (!fs) {
        return GW_FRESET_BOOT_NO_FS;
    }
    return file ? GW_FRESET_BOOT_FILE : GW_FRESET_BOOT_EXAMPLE;
}
