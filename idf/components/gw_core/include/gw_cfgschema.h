/*
 * FW-1 (issue #144 item 3): the configuration schema that a stored document needs.
 *
 * Each configuration key and enum value has a "since" schema number: the first schema (and so
 * the first firmware) that reads it. The validator (gw_config.c) gives every key it sees, and
 * every enum value it accepts, to gw_schema_note() with its JSON path. The report then holds the
 * highest number that the document needs, the count of paths per number, and the paths with the
 * highest numbers. Before an update writes an image, gw_schema_fits() compares the report with
 * the schema that the image declares (its capabilities block, gw_ota.h): an image that cannot
 * read the document is refused, with the JSON paths that are too new.
 *
 * The table of "since" numbers lists only the keys and values that came after schema 1 (all the
 * others are 1). Its paths use "[]" for an array index: "devices[].tags[].scan".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SCHEMA_TOP 3     /* paths kept for a refusal (the highest "since" numbers) */
#define GW_SCHEMA_PATH 64   /* with the NUL; a longer path is cut */
#define GW_SCHEMA_HIST 16   /* paths counted per number; numbers from 15 up share the last */

typedef struct {
    const char *path;       /* "devices[].tags[].scan" */
    const char *value;      /* an enum value of that key, NULL: the key itself */
    uint16_t since;
} gw_since_t;

typedef struct {
    uint16_t since;
    char path[GW_SCHEMA_PATH];
} gw_schema_path_t;

typedef struct {
    const gw_since_t *table;
    size_t n;
    bool valid;             /* a whole document was examined (the parse accepted it) */
    uint16_t need;          /* the highest number seen; 0: no key at all */
    uint32_t count;         /* paths seen */
    uint16_t hist[GW_SCHEMA_HIST];
    gw_schema_path_t top[GW_SCHEMA_TOP];   /* highest numbers first; since 0: unused */
} gw_schema_report_t;

/** An empty report (not valid) that looks up the numbers in @p table (@p n entries). */
void gw_schema_init(gw_schema_report_t *r, const gw_since_t *table, size_t n);

/** True when @p path matches @p pattern, where "[]" in the pattern matches any "[N]". */
bool gw_schema_path_match(const char *pattern, const char *path);

/** The "since" number of the key at @p path (@p value NULL) or of its enum value @p value:
 *  the table entry, or 1 when the table does not list it. */
uint16_t gw_schema_since(const gw_since_t *table, size_t n, const char *path, const char *value);

/** Record the key at @p path (@p value NULL) or its enum value. */
void gw_schema_note(gw_schema_report_t *r, const char *path, const char *value);

/** Paths of the report with a number above @p schema. */
uint32_t gw_schema_newer(const gw_schema_report_t *r, uint16_t schema);

/** True when an image that reads up to @p schema can read the document. Otherwise @p why lists
 *  the paths that are too new: "configuration needs schema 3, the image reads 2: a.b (3),
 *  c[0].d (3) and 4 more". */
bool gw_schema_fits(const gw_schema_report_t *r, uint16_t schema, char *why, size_t cap);

#ifdef __cplusplus
}
#endif
