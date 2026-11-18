/*
 * Issue #226: helpers shared by the libFuzzer targets of gw_core.
 *
 * Each target reads its input with a small cursor (fz_t). A read past the end gives zeros, so
 * every input, also an empty one, is a valid test case. Strings and buffers that a function
 * under test reads go to a heap block of their exact size: AddressSanitizer then finds a read
 * of one byte past them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const uint8_t *p;
    size_t n;
} fz_t;

static inline uint8_t fz_u8(fz_t *f)
{
    if (f->n == 0) {
        return 0;
    }
    f->n--;
    return *f->p++;
}

static inline uint16_t fz_u16(fz_t *f)
{
    uint16_t hi = fz_u8(f);
    return (uint16_t)((hi << 8) | fz_u8(f));
}

static inline uint32_t fz_u32(fz_t *f)
{
    uint32_t hi = fz_u16(f);
    return (hi << 16) | fz_u16(f);
}

static inline bool fz_bool(fz_t *f)
{
    return (fz_u8(f) & 1u) != 0;
}

/* Up to @p max bytes of the input (fewer at its end): a pointer into the input. */
static inline const uint8_t *fz_bytes(fz_t *f, size_t max, size_t *len)
{
    size_t n = max < f->n ? max : f->n;
    const uint8_t *p = f->p;
    f->p += n;
    f->n -= n;
    *len = n;
    return p;
}

/* A heap copy of exactly @p len bytes (one byte for len 0, so that the pointer is not NULL). */
static inline uint8_t *fz_dup(const uint8_t *p, size_t len)
{
    uint8_t *d = malloc(len != 0 ? len : 1);
    if (d == NULL) {
        abort();
    }
    if (len != 0) {
        memcpy(d, p, len);
    }
    return d;
}

/* A NUL-terminated heap copy of @p len bytes (a NUL byte in them ends the C string early). */
static inline char *fz_strdup(const uint8_t *p, size_t len)
{
    char *s = malloc(len + 1);
    if (s == NULL) {
        abort();
    }
    if (len != 0) {
        memcpy(s, p, len);
    }
    s[len] = '\0';
    return s;
}

/* The next string of the input: the bytes up to a NUL byte (not included) or to the end. */
static inline char *fz_cstr(fz_t *f)
{
    const uint8_t *end = memchr(f->p, 0, f->n);
    size_t len = end != NULL ? (size_t)(end - f->p) : f->n;
    char *s = fz_strdup(f->p, len);
    size_t used = end != NULL ? len + 1 : len;
    f->p += used;
    f->n -= used;
    return s;
}

/* The rest of the input. */
static inline const uint8_t *fz_rest(fz_t *f, size_t *len)
{
    return fz_bytes(f, f->n, len);
}

/* Read a whole file at start-up (the configurations of the repository). Aborts on error. */
static inline char *fz_read_file(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        abort();
    }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL || n < 0 || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        abort();
    }
    fclose(fp);
    buf[n] = '\0';
    *len = (size_t)n;
    return buf;
}

/* An invariant of the code under test does not hold: stop with a crash that libFuzzer saves. */
#define FZ_CHECK(cond)                                                                    \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);      \
            abort();                                                                      \
        }                                                                                 \
    } while (0)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
