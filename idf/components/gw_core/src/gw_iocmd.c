#include "gw_iocmd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "gw_config.h"
#include "gw_config_priv.h"
#include "gw_port.h"

static bool parse_root(const cJSON *root, uint8_t *index, bool *on, char *why, size_t cap)
{
    if (!cJSON_IsObject(root)) {
        snprintf(why, cap, "expected a JSON object {\"do\": 1-8, \"on\": true|false}");
        return false;
    }
    for (const cJSON *it = root->child; it != NULL; it = it->next) {
        if (strcmp(it->string, "do") != 0 && strcmp(it->string, "on") != 0) {
            snprintf(why, cap, "%s: unknown key", it->string);
            return false;
        }
    }
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "do");
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "on");
    if (!cJSON_IsNumber(d) || d->valuedouble != floor(d->valuedouble) || d->valuedouble < 1 ||
        d->valuedouble > GW_LOCAL_IO_CHANNELS) {
        snprintf(why, cap, "do: expected the number of the output, 1 to %d", GW_LOCAL_IO_CHANNELS);
        return false;
    }
    if (!cJSON_IsBool(o)) {
        snprintf(why, cap, "on: expected true or false");
        return false;
    }
    *index = (uint8_t)(d->valuedouble - 1);
    *on = cJSON_IsTrue(o);
    return true;
}

bool gw_iocmd_parse(const char *json, size_t len, uint8_t *index, bool *on, char *why, size_t cap)
{
    gw_port_json_lock();
    gw_config_json_hooks();
    cJSON *root = cJSON_ParseWithLength(json, len);
    bool ok;
    if (root == NULL) {
        snprintf(why, cap, gw_config_json_alloc_failed() ? "out of memory parsing the request"
                                                         : "the request is not valid JSON");
        ok = false;
    } else {
        ok = parse_root(root, index, on, why, cap);
    }
    cJSON_Delete(root);
    gw_port_json_unlock();
    return ok;
}
