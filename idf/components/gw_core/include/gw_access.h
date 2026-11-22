/*
 * Issue #280: the answer of the shell command 'access', pure C (host-tested).
 *
 * The command gives, on request, what a person needs to open the web UI: the mode, the firmware
 * version, the state and the address of each interface, the URL of the web UI, the login and the
 * fingerprint of the HTTPS certificate. The console showed the commissioning password and the
 * fingerprint only at the boot; the install tool of #219 opens the console later.
 *
 * The answer has a fixed form, so that the tool reads it as data. Each line is "key value",
 * between two marker lines:
 *
 *   access-data v1
 *   mode commissioning                  normal | recovery | commissioning | fault | starting
 *   firmware 0.2.0
 *   hostname gw01                       absent when not known
 *   web on https 443                    or "web off"
 *   login commissioning                 commissioning | web-password | web-password-any-user
 *   user admin                          absent with web-password-any-user
 *   commissioning-password k7Qm2xPa9TbE only with "login commissioning"
 *   fingerprint 4F:9A:...               SHA-256 of the HTTPS certificate; absent without HTTPS
 *   if eth up 192.168.1.57              off | no-link | no-address | up <IPv4>, for each interface
 *   if wifi off
 *   if ap off
 *   url https://192.168.1.57/           one line for each address; then the mDNS name
 *   url https://gw01.local/
 *   access-data end
 *
 * With no URL, one line gives the cause: "url-none web-off" or "url-none no-address".
 * A new key can come in a later firmware: a reader ignores a key that it does not know. A change
 * of the meaning of a key changes the number after "v".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_ACCESS_VERSION 1
#define GW_ACCESS_IF_MAX 3
/* The longest answer: 3 interfaces, 4 URLs, the longest names. */
#define GW_ACCESS_TEXT_MAX 1024

typedef enum {
    GW_ACCESS_IF_OFF = 0,      /* not configured, or the access point does not run */
    GW_ACCESS_IF_NO_LINK,
    GW_ACCESS_IF_NO_ADDRESS,
    GW_ACCESS_IF_UP,
} gw_access_if_state_t;

typedef struct {
    const char *name;          /* "eth", "wifi", "ap" */
    gw_access_if_state_t state;
    uint8_t ip[4];             /* with GW_ACCESS_IF_UP */
    bool no_mdns;              /* the mDNS name does not answer there (the access point) */
} gw_access_if_t;

typedef struct {
    const char *mode;
    const char *firmware;
    const char *hostname;                /* "" or NULL: not known */
    bool mdns;                           /* <hostname>.local answers */
    bool web_running;
    bool https;
    uint16_t port;                       /* of the web UI */
    bool password_set;                   /* a web password is set */
    const char *username;                /* with password_set: "" or NULL = any user name */
    const char *commissioning_password;  /* given only without a web password */
    const char *fingerprint;             /* "" or NULL: none */
    gw_access_if_t ifs[GW_ACCESS_IF_MAX];
    size_t n_ifs;
} gw_access_t;

/**
 * Write the answer to @p out (NUL-terminated, lines end with "\n"). Returns its length, as
 * snprintf: a value >= @p cap means that the text was cut. A control character of a value from
 * the configuration becomes '?', so that a value cannot make a line.
 */
size_t gw_access_format(const gw_access_t *a, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
