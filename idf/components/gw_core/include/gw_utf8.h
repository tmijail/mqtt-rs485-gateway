/*
 * UTF-8 checks for the text that leaves the gateway in MQTT topics and Sparkplug strings
 * (issue #162, CFG-1): a broker may refuse a topic that is not well-formed UTF-8 and close the
 * connection (MQTT 3.1.1 section 1.5.3, MQTT 5 section 1.5.4), and a protobuf decoder may refuse
 * a string field that is not UTF-8.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Length (1-4) of the well-formed UTF-8 sequence that starts at @p s, looking at most at @p n
 * bytes; 0 if the bytes there are not one (RFC 3629: no overlong forms, no surrogates, nothing
 * above U+10FFFF, no truncated sequence). A NUL byte is a sequence of length 1.
 */
size_t gw_utf8_seq(const char *s, size_t n);

/** true if the NUL-terminated @p s is well-formed UTF-8. */
bool gw_utf8_valid(const char *s);

/** The code point of the well-formed sequence of length @p len (from gw_utf8_seq) at @p s. */
unsigned long gw_utf8_code(const char *s, size_t len);

#ifdef __cplusplus
}
#endif
