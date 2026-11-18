/* Issue #228: the self-test of check_jsonlock. bad_json_parse (in the public header of the
 * self-test) reaches cJSON without the lock, and bad_json_locked never releases the lock.
 * good_json_parse holds the lock around its helper, and good_json_helper has only that caller.
 * Each name with "good_" must not be in the report. */
#include <stddef.h>

#include "cJSON.h"
#include "gw_port.h"

__attribute__((noinline)) static int parse_unlocked(const char *s)
{
    cJSON *root = cJSON_Parse(s);
    int ok = root != NULL;
    cJSON_Delete(root);
    return ok;
}

int bad_json_parse(const char *s)
{
    return parse_unlocked(s);
}

int bad_json_locked(const char *s)
{
    gw_port_json_lock();
    return parse_unlocked(s);
}

__attribute__((noinline)) static int good_json_helper(const char *s)
{
    cJSON *root = cJSON_Parse(s);
    int ok = cJSON_IsObject(root);
    cJSON_Delete(root);
    return ok;
}

int good_json_parse(const char *s)
{
    gw_port_json_lock();
    int ok = good_json_helper(s);
    gw_port_json_unlock();
    return ok;
}
