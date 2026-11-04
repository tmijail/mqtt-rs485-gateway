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

#ifdef __cplusplus
}
#endif
