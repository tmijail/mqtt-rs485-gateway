/* Internal to gw_core: the parser's memory hooks, shared by gw_config and gw_cfgdoc. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Install the configured allocator as cJSON's hooks and clear the failure flag. */
void gw_config_json_hooks(void);

/** True if an allocation failed since the last gw_config_json_hooks(). */
bool gw_config_json_alloc_failed(void);

/** The allocator set with gw_config_set_allocator (default malloc/free). */
void *gw_config_alloc(size_t size);
void gw_config_release(void *ptr);
