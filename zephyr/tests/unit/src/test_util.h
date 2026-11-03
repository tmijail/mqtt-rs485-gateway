/*
 * Shared helpers for the gateway unit tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_TEST_UTIL_H_
#define GW_TEST_UTIL_H_

#include "gw_config.h"

/* The example document of SPEC section 6, verbatim. */
extern const char spec_config_json[];

/* Parse @p json into the shared test configuration; asserts on failure. */
struct gw_config *test_config_load(const char *json);

/* Parse @p json and return the error text; asserts that parsing fails. */
const char *test_config_error(const char *json);

#endif /* GW_TEST_UTIL_H_ */
