/* POSIX implementation of gw_port.h, used by the host unit tests. */
#include "gw_port.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_json_lock;
static pthread_once_t s_json_once = PTHREAD_ONCE_INIT;
static char *s_capture;
static size_t s_capture_cap;
static uint64_t s_fake_ms;   /* 0: real clock */

void gw_port_fake_time(uint64_t ms)
{
    s_fake_ms = ms;
}

void gw_port_log_capture(char *buf, size_t cap)
{
    s_capture = buf;
    s_capture_cap = cap;
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
}

void gw_port_init(void)
{
}

uint64_t gw_port_time_ms(void)
{
    if (s_fake_ms != 0) {
        return s_fake_ms;
    }
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

static void json_lock_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_json_lock, &a);
    pthread_mutexattr_destroy(&a);
}

void gw_port_json_lock(void)
{
    pthread_once(&s_json_once, json_lock_init);   /* host tests may not call gw_port_init */
    pthread_mutex_lock(&s_json_lock);
}

void gw_port_json_unlock(void)
{
    pthread_mutex_unlock(&s_json_lock);
}

void gw_port_log(char level, const char *tag, const char *fmt, ...)
{
    /* Quiet by default so test output stays readable; GW_TEST_VERBOSE=1 shows everything. */
    static int verbose = -1;
    if (verbose < 0) {
        const char *v = getenv("GW_TEST_VERBOSE");
        verbose = (v != NULL && v[0] == '1');
    }
    if (s_capture != NULL) {
        size_t used = strlen(s_capture);
        if (used + 1 < s_capture_cap) {
            int n = snprintf(s_capture + used, s_capture_cap - used, "%c (%s) ", level, tag);
            used += n > 0 ? (size_t)n : 0;
            if (used + 1 < s_capture_cap) {
                va_list cp;
                va_start(cp, fmt);
                n = vsnprintf(s_capture + used, s_capture_cap - used, fmt, cp);
                va_end(cp);
                used += n > 0 ? (size_t)n : 0;
            }
            if (used + 1 < s_capture_cap) {
                s_capture[used] = '\n';
                s_capture[used + 1] = '\0';
            }
        }
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
