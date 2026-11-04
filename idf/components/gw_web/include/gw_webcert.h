/* Issue #64: the web server's device certificate (see gw_webcert.c). */
#pragma once

#include <stdbool.h>

#define GW_WEBCERT_PEM_MAX 1536
#define GW_WEBCERT_KEY_MAX 512

/**
 * The device certificate and key (PEM), generated and stored in NVS the first time. Logs the
 * SHA-256 fingerprint. False if there is none and it could not be generated. The strings live
 * for the whole run.
 */
bool gw_webcert_load(const char *hostname, const char **cert, const char **key);
