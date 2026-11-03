/* POSIX implementation of gw_port.h, used by the host unit tests. */
#include "gw_port.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void gw_port_init(void)
{
}

uint64_t gw_port_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)(tv.tv_usec / 1000);
}

void gw_port_lock(void)
{
    pthread_mutex_lock(&s_lock);
}

void gw_port_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

void gw_port_log(char level, const char *tag, const char *fmt, ...)
{
    /* Quiet by default so test output stays readable; GW_TEST_VERBOSE=1 shows everything. */
    static int verbose = -1;
    if (verbose < 0) {
        const char *v = getenv("GW_TEST_VERBOSE");
        verbose = (v != NULL && v[0] == '1');
    }
    if (!verbose && level != 'E') {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%c (%s) ", level, tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
