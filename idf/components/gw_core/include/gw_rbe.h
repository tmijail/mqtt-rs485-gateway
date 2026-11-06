/*
 * Report by exception with absolute deadband (SPB-7, SPB-12).
 *
 * The reference is the last *reported* value, so slow drifts are still reported
 * once they exceed the deadband.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double value;
    uint64_t key;    /* exact variant: 64-bit integer bits or string hash */
    bool valid;
    bool reported;   /* false until the first report (birth) */
} gw_rbe_t;

/** true if (@p value, @p valid) must be reported. A deadband <= 0 means "any change". */
bool gw_rbe_changed(const gw_rbe_t *state, double value, bool valid, double deadband);

/** Record that (@p value, @p valid) has been reported. */
void gw_rbe_commit(gw_rbe_t *state, double value, bool valid);

/**
 * Exact variant for 64-bit integers (MB-6), where a double would hide changes above 2^53:
 * @p key holds the value's bits. A deadband > 0 compares the distance between the two values,
 * read as signed integers when @p is_signed. Strings pass a hash of the text (gw_rbe_hash)
 * and no deadband.
 */
bool gw_rbe_changed_key(const gw_rbe_t *state, uint64_t key, bool is_signed, bool valid,
                        double deadband);
void gw_rbe_commit_key(gw_rbe_t *state, uint64_t key, bool valid);

/** 64-bit FNV-1a of a NUL-terminated string. */
uint64_t gw_rbe_hash(const char *s);

/** 64-bit FNV-1a of @p len bytes (raw byte values, SPB-19). */
uint64_t gw_rbe_hash_bytes(const void *data, size_t len);

#ifdef __cplusplus
}
#endif
