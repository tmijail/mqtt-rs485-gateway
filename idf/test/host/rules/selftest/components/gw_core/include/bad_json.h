/* Issue #228: the public header of the core component of the self-test (check_jsonlock). The
 * other components can call each function of a public header, so each is an entry. */
#pragma once

int bad_json_parse(const char *s);
int bad_json_locked(const char *s);
int good_json_parse(const char *s);
