/*
 * #204: a DO command from the web UI (POST /api/do {"do": 1-8, "on": true|false}). The web task
 * gives it to the local_io task through the same queue as an NCMD (gw_io_command_do), so the
 * rules of a DCMD apply: a DO that a local rule drives is refused, and the new state goes to the
 * Sparkplug host in the next NDATA.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Parse the body @p json (@p len bytes): @p index 0-7 and @p on. False with the reason in
 *  @p why (for the answer 400). */
bool gw_iocmd_parse(const char *json, size_t len, uint8_t *index, bool *on, char *why, size_t cap);

#ifdef __cplusplus
}
#endif
