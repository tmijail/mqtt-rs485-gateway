/*
 * Login rate limit per source address for the web UI and REST API (issue #64).
 *
 * Pure state machine, no OS calls. Every failed login of a source address counts; after
 * max_fails failures in a row the address is blocked for block_ms, and every further block of
 * the same address in a row doubles the time, up to block_max_ms. A blocked address gets no
 * credential check at all (not even with the right password, or guessing would go on). A
 * successful login forgets the address. The table is small and fixed: when it is full, the
 * entry least recently seen is reused (a blocked one only if every entry is blocked).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_AUTHLIMIT_SLOTS 8

typedef struct {
    uint8_t addr[16];          /* IPv4 in the first 4 bytes, or IPv6 */
    bool used;
    uint8_t fails;
    uint32_t next_block_ms;
    uint64_t blocked_until_ms;
    uint64_t seen_ms;
} gw_authlimit_slot_t;

typedef struct {
    gw_authlimit_slot_t slot[GW_AUTHLIMIT_SLOTS];
    uint8_t max_fails;
    uint32_t block_ms, block_max_ms;
    uint32_t failed_total;     /* failed logins */
    uint32_t refused_total;    /* requests refused while blocked */
    uint32_t blocks_total;     /* times an address was blocked */
} gw_authlimit_t;

void gw_authlimit_init(gw_authlimit_t *l, uint8_t max_fails, uint32_t block_ms,
                       uint32_t block_max_ms);

/** 0 if @p addr may try to log in at @p now_ms, else the milliseconds it is still blocked
 *  (the request is counted as refused). */
uint32_t gw_authlimit_check(gw_authlimit_t *l, const uint8_t addr[16], uint64_t now_ms);

/** A login from @p addr failed. Returns the block time it starts now (0 = not blocked). */
uint32_t gw_authlimit_failed(gw_authlimit_t *l, const uint8_t addr[16], uint64_t now_ms);

/** A login from @p addr succeeded: forget it. */
void gw_authlimit_succeeded(gw_authlimit_t *l, const uint8_t addr[16]);

/** Addresses blocked at @p now_ms. */
unsigned gw_authlimit_blocked(const gw_authlimit_t *l, uint64_t now_ms);

/*
 * Issue #110: stall watchdog of a single-task web server (esp_http_server). The server task
 * marks @p alive_ms every time it gets back to its loop; @p busy is true while it runs one of
 * our handlers (a long upload, a validation) or drains the body of a refused authenticated
 * request, which may legitimately take long. Otherwise a task that has not been back for more
 * than @p limit_ms is held by a request that does not arrive: a TLS handshake or headers sent
 * byte by byte (slowloris), or a client that stopped half-way. Wrap-safe 32-bit milliseconds;
 * an @p alive_ms after @p now_ms (the server marked itself alive after the read of the clock)
 * is not a stall.
 */
bool gw_webstall_stuck(uint32_t now_ms, uint32_t alive_ms, bool busy, uint32_t limit_ms);

/*
 * Issue #291: the sessions of a held server that the watchdog shuts down. @p handshake[k] is
 * true for session k while it is in its TLS handshake (it has no TLS context yet).
 * esp_https_server does the handshake in the accept, in the task of the server: a session in its
 * handshake is the one that holds the task, and only it goes. Otherwise the watchdog cannot
 * tell which session holds the task, and all go. Sets @p shut[k] for each session to shut down
 * and returns their number.
 */
size_t gw_webstall_pick(const bool *handshake, size_t n, bool *shut);

#ifdef __cplusplus
}
#endif
