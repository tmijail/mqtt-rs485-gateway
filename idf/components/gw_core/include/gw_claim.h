/*
 * CFG-3 / NET-5, issue #120: the claim window. The first web password of a gateway, set
 * without the serial console and without the commissioning password.
 *
 *   - The window opens only on the BOOT gesture of 2-10 s (gw_button.h) and only while no web
 *     password is set. With a web password set it never opens: only a factory reset (CFG-5)
 *     removes the password.
 *   - It closes at the end of its time, after the first password that is set through it, and
 *     at a restart (RAM only). A second BOOT gesture while it is open starts its time again.
 *   - Only a client of the commissioning access point may use it (gw_claim_from_ap): a person
 *     on the plant network cannot take the gateway while the window is open.
 *   - The new password obeys the rule of web.password in the configuration (at least 8
 *     characters) and the size of the field (GW_CRED_SIZE - 1), with no control characters and
 *     well-formed UTF-8 (it is typed into a browser and sent in HTTP Basic).
 *
 * Pure logic, no platform calls: the caller gives the time (ms) and the interface data.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GW_CLAIM_PW_MIN 8
#define GW_CLAIM_PW_MAX (GW_CRED_SIZE - 1)
/* The largest request body of a claim: {"password": "<64 characters, each escaped>"} */
#define GW_CLAIM_BODY_MAX 512

typedef struct {
    bool open;
    uint64_t until_ms;    /* end of the window (valid while open) */
    uint32_t opened;      /* times the window opened since boot */
    uint32_t claimed;     /* passwords set through the window (0 or 1 in one boot) */
} gw_claim_t;

typedef enum {
    GW_CLAIM_OK = 0,
    GW_CLAIM_PASSWORD_SET,   /* a web password is set: the window never opens */
    GW_CLAIM_CLOSED,         /* no window: never opened, time over, or used */
    GW_CLAIM_NOT_AP,         /* the client is not a client of the access point */
    GW_CLAIM_BAD_REQUEST,    /* not {"password": "<string>"} */
    GW_CLAIM_TOO_SHORT,
    GW_CLAIM_TOO_LONG,
    GW_CLAIM_BAD_CHAR,       /* a control character, or not well-formed UTF-8 */
} gw_claim_result_t;

void gw_claim_init(gw_claim_t *c);

/**
 * The BOOT gesture: open the window for @p duration_ms from @p now_ms (or start its time
 * again). Refused (false, nothing changes) when @p password_set or @p duration_ms is 0.
 */
bool gw_claim_open(gw_claim_t *c, bool password_set, uint32_t duration_ms, uint64_t now_ms);

/** True while the window is open at @p now_ms. At the end of its time it closes here. */
bool gw_claim_active(gw_claim_t *c, uint64_t now_ms);

/** Milliseconds left at @p now_ms (0 when closed). */
uint32_t gw_claim_left_ms(const gw_claim_t *c, uint64_t now_ms);

/** Close the window (end of its time from a timer, or the access point is not available). */
void gw_claim_close(gw_claim_t *c);

/**
 * May the @p password (NUL terminated, from gw_claim_parse) be set now? The order of the
 * checks: a set password, the window, the access point rule, then the password itself.
 * GW_CLAIM_OK does not close the window: call gw_claim_done() when the password is stored.
 */
gw_claim_result_t gw_claim_check(gw_claim_t *c, bool password_set, bool from_ap,
                                 const char *password, uint64_t now_ms);

/** The password from the window is stored: the window closes and does not open again. */
void gw_claim_done(gw_claim_t *c);

/**
 * The body of POST /api/claim: {"password": "..."} (other keys are refused). The password
 * goes to @p pw (at most @p cap - 1 bytes; a longer one gives GW_CLAIM_TOO_LONG). Returns
 * GW_CLAIM_OK, GW_CLAIM_BAD_REQUEST, GW_CLAIM_TOO_LONG or GW_CLAIM_BAD_CHAR (a "\u0000" escape,
 * which would cut the password). Takes gw_port_json_lock().
 */
gw_claim_result_t gw_claim_parse(const char *json, size_t len, char *pw, size_t cap);

/** One English sentence for @p r, for the API answer and the log. */
const char *gw_claim_text(gw_claim_result_t r);

/** An IPv4 interface: address and netmask (network byte order, as in a struct in_addr). */
typedef struct {
    uint8_t ip[4];
    uint8_t mask[4];
} gw_claim_if_t;

/**
 * The access point rule. True only when the TCP connection is local to the access point:
 *   - @p ap has an address (not 0.0.0.0) and a netmask;
 *   - the local address of the connection (@p local) is the address of the access point;
 *   - the peer (@p peer) is a host of the subnet of the access point (not its network,
 *     broadcast or own address);
 *   - no other interface (@p others, @p n_others; an interface without an address is ignored)
 *     has a subnet that contains the peer: the route back to the peer is not ambiguous.
 * An IPv6 connection is not given to this function: the access point has IPv4 only.
 * The lwIP of ESP-IDF routes by the source address (ip4_route_src_hook): the answers to a
 * connection on the address of the access point always go out through the access point, so a
 * host on another interface that sends to that address gets no answer. The peer check is a
 * second barrier.
 */
bool gw_claim_from_ap(const uint8_t local[4], const uint8_t peer[4], const gw_claim_if_t *ap,
                      const gw_claim_if_t *others, size_t n_others);

#ifdef __cplusplus
}
#endif
