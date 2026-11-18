/* Issue #228: the self-test of check_noalloc (rules/selftest/noalloc_path.txt lists this file).
 * bad_poll allocates through a function of another file (the call graph finds it), and
 * bad_drop calls free (nm finds it too). Each name with "good_" must not be in the report. */
#include <stddef.h>
#include <stdlib.h>

void *bad_alloc_helper(size_t n);
int good_helper(int x);

void *bad_poll(void)
{
    return bad_alloc_helper(16);
}

void bad_drop(void *p)
{
    free(p);
}

int good_poll(int x)
{
    return good_helper(x) + 1;
}
