/*
 * REL-1 / issue #132: the core dump that a panic leaves in the "coredump" partition, for the
 * web API (GET/DELETE /api/coredump, GET /api/coredump/summary, "core_dump" in /api/status),
 * the shell ('coredump', 'coredump dump', 'coredump erase') and the NBIRTH metric
 * "Node Info/Core Dump".
 *
 * This module holds what is pure logic: the state of the dump, its summary as JSON and as
 * text, and the text of 'coredump dump' (#293). gw_storage (gw_coredump.c) reads the
 * partition with ESP-IDF's esp_core_dump_* calls and fills these structures. No OS calls, no
 * allocation.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_auth.h"   /* gw_sha256_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_CDUMP_NONE = 0,   /* the partition is blank (or there is no partition) */
    GW_CDUMP_VALID,      /* a dump whose checksum is correct */
    GW_CDUMP_CORRUPT,    /* a dump with a wrong checksum or size: download it, erase it */
} gw_cdump_state_t;

#define GW_CDUMP_BT_MAX     16   /* esp_core_dump_bt_info_t.bt */
#define GW_CDUMP_TASK_SIZE  16   /* configMAX_TASK_NAME_LEN with its NUL */
#define GW_CDUMP_SHA_SIZE   65   /* the hex SHA-256 of the app's ELF, as far as the dump has it */
#define GW_CDUMP_REASON_SIZE 128

/* 'coredump dump': 57 bytes of the dump per line, 76 base64 characters (RFC 2045 lines). Every
 * line but the last encodes a multiple of 3 bytes, so the lines join into one base64 text, and
 * esp-coredump (--core-format b64) also decodes them line by line. */
#define GW_CDUMP_LINE_BYTES 57
#define GW_CDUMP_LINE_CHARS 76

typedef struct {
    gw_cdump_state_t state;
    uint32_t size;               /* bytes of the dump with its checksum, 0 = unknown */
    /* The summary (esp_core_dump_get_summary), only for a valid dump that was read: */
    bool has_summary;
    char task[GW_CDUMP_TASK_SIZE];   /* the task that crashed */
    uint32_t pc;                 /* its program counter at the exception */
    uint32_t bt[GW_CDUMP_BT_MAX];
    uint8_t depth;               /* entries of bt */
    bool bt_corrupted;           /* the backtrace stopped at a corrupt frame */
    uint32_t exc_cause;          /* Xtensa EXCCAUSE */
    uint32_t exc_vaddr;          /* Xtensa EXCVADDR */
    char app_sha256[GW_CDUMP_SHA_SIZE];   /* "" = unknown */
    char reason[GW_CDUMP_REASON_SIZE];    /* the panic reason, "" = not in the dump */
} gw_cdump_info_t;

/** "none", "valid" or "corrupt". */
const char *gw_cdump_state_name(gw_cdump_state_t s);

/** The "core_dump" object of /api/status: {"present":..,"state":..,"size":..}. Returns the
 *  length, or 0 when it does not fit in @p cap (then @p out is "{}" if it has room). */
size_t gw_cdump_status_json(gw_cdump_state_t state, uint32_t size, char *out, size_t cap);

/** GET /api/coredump/summary: the status fields, @p reset_reason (of this boot: the dump can
 *  be older) and, with a summary, task, pc, backtrace, backtrace_corrupted, exc_cause,
 *  exc_vaddr, reason and app_elf_sha256. Addresses are "0x%08x" strings. Returns the length,
 *  or 0 when it does not fit (then @p out is "{}" if it has room). */
size_t gw_cdump_summary_json(const gw_cdump_info_t *i, const char *reset_reason, char *out,
                             size_t cap);

/** The shell's summary: lines that end with '\n'. Returns the length (truncated to @p cap). */
size_t gw_cdump_text(const gw_cdump_info_t *i, const char *reset_reason, char *out, size_t cap);

/** Standard base64 (RFC 4648) of @p in[0..len), with '=' padding, NUL terminated. Returns the
 *  length without the NUL, or 0 when it does not fit in @p cap (4 * ceil(len / 3) + 1). */
size_t gw_cdump_b64(const uint8_t *in, size_t len, char *out, size_t cap);

/* ---------- 'coredump dump' (#132, #293) ----------
 * The text of the copy:
 *
 *   coredump: <size> bytes (<state>), <lines> lines of base64, sha256 <64 hex digits>
 *   coredump: the dump can contain secrets (passwords, keys): keep the copy safe
 *   -----BEGIN CORE DUMP-----
 *   <lines> lines of base64 (GW_CDUMP_LINE_BYTES bytes each, the last one shorter)
 *   -----END CORE DUMP----- <lines> lines
 *
 * The base64 lines are as before #293: they join into the dump, and esp-coredump
 * (--core-format b64) decodes them. The first line and the END marker let a reader find a copy
 * with a lost line (#293: stdout on the USB-Serial-JTAG console of ESP-IDF drops output; the
 * shell thus writes the copy into the driver, see gw_shell.c): the number of lines, and the size
 * and SHA-256 of the decoded bytes.
 *
 * A copy that stops has no END marker. It ends with one line instead:
 *   -----CORE DUMP INCOMPLETE: <reason>-----
 *
 * The SHA-256 comes from a first read of the whole dump; the lines come from a second read. When
 * the bytes of the two reads are not the same (an erase during the copy), the copy stops at the
 * end with GW_CDUMP_COPY_CHANGED. */
#define GW_CDUMP_READ_LINES  16   /* lines for each read of the partition: 912 bytes */
/* Lines for each write to the console: 3 * (76 + "\r\n") = 234 bytes. A write holds the console
 * lock, so the log of other tasks waits a short time only. */
#define GW_CDUMP_WRITE_LINES 3
#define GW_CDUMP_TEXT_SIZE   256  /* the text of one write: GW_CDUMP_WRITE_LINES lines, or one line */

typedef enum {
    GW_CDUMP_COPY_OK = 0,
    GW_CDUMP_COPY_READ,      /* a read of the partition failed */
    GW_CDUMP_COPY_CHANGED,   /* the two reads gave different bytes */
    GW_CDUMP_COPY_STALLED,   /* the console did not take the text (io.write returned false) */
} gw_cdump_copy_t;

typedef struct {
    void *ctx;
    /** Read @p len bytes of the dump at @p off. Returns NULL, or the name of the error. */
    const char *(*read)(void *ctx, uint32_t off, uint8_t *buf, size_t len);
    /** Write @p len characters (complete lines) to the console. Returns false when the reader
     *  did not take the earlier text in time: then nothing was written, and the copy stops. */
    bool (*write)(void *ctx, const char *text, size_t len);
} gw_cdump_io_t;

/** The memory of one copy: the caller allocates it (it is too large for the stack of the
 *  console task). */
typedef struct {
    uint8_t bin[GW_CDUMP_READ_LINES * GW_CDUMP_LINE_BYTES];
    char text[GW_CDUMP_TEXT_SIZE];
    gw_sha256_t sha;
} gw_cdump_work_t;

/** The number of base64 lines for a dump of @p size bytes. */
uint32_t gw_cdump_line_count(uint32_t size);

/** Write the copy of a dump of @p size bytes (> 0) in state @p state through @p io, as the
 *  comment above shows. After a failure, it writes the INCOMPLETE line (the write can fail
 *  also). The work memory is zeroed at the end (the dump can hold secrets). */
gw_cdump_copy_t gw_cdump_copy(const gw_cdump_io_t *io, gw_cdump_state_t state, uint32_t size,
                              gw_cdump_work_t *w);

#ifdef __cplusplus
}
#endif
