/* Issue #228: the self-test of check_noalloc. This file is not in the list: only the functions
 * that the listed file calls are examined. good_unlisted_alloc is not on the path. */
#include <stddef.h>
#include <stdlib.h>

void *bad_alloc_helper(size_t n)
{
    return calloc(n, 1);
}

int good_helper(int x)
{
    return x * 2;
}

void *good_unlisted_alloc(size_t n)
{
    return malloc(n);
}
