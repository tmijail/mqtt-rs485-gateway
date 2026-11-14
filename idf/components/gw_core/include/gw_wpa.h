/*
 * NET-5, issue #187: the WPA2 key of the commissioning access point is the web password.
 *
 * The gateway keeps only the pairwise master key (PMK) of the web password, never the password:
 *   PMK = PBKDF2-HMAC-SHA1(passphrase, SSID, 4096 iterations, 32 bytes)   (IEEE 802.11i H.4)
 * The access point gets the PMK as 64 hexadecimal characters (the field wifi_config_t.ap.password
 * has 64 bytes: copied without a NUL). WPA2-PSK only: WPA3 (SAE) needs the passphrase itself.
 *
 * The passphrase rule of WPA2 (hostapd, wpa_supplicant): 8 to 63 characters, each one printable
 * ASCII (0x20 to 0x7E). The same rule applies to web.password (gw_config) and to the claim
 * window (gw_claim), so each web password is also a valid WPA2 passphrase.
 *
 * The SSID comes from the MAC address ("GW-Setup-XXXXXX", gw_wpa_ssid), so a stored PMK stays
 * correct. Pure C, no platform calls; SHA-1 is here because the PMK is its only user.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GW_SHA1_LEN          20
#define GW_WPA_PASS_MIN      8
#define GW_WPA_PASS_MAX      63
#define GW_WPA_ITERATIONS    4096
#define GW_WPA_PMK_LEN       32
#define GW_WPA_PMK_HEX_SIZE  (2 * GW_WPA_PMK_LEN + 1)   /* 64 hexadecimal characters + NUL */
#define GW_WPA_SSID_MAX      32
#define GW_WPA_SSID_SIZE     (GW_WPA_SSID_MAX + 1)

typedef enum {
    GW_WPA_PASS_OK = 0,
    GW_WPA_PASS_SHORT,      /* fewer than GW_WPA_PASS_MIN characters */
    GW_WPA_PASS_LONG,       /* more than GW_WPA_PASS_MAX characters */
    GW_WPA_PASS_BAD_CHAR,   /* a character outside 0x20..0x7E (control, DEL, not ASCII) */
} gw_wpa_pass_t;

/** The passphrase rule for @p pw (NUL terminated). The length is checked first. */
gw_wpa_pass_t gw_wpa_pass_check(const char *pw);

void gw_sha1(const void *data, size_t len, uint8_t out[GW_SHA1_LEN]);

void gw_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                  uint8_t out[GW_SHA1_LEN]);

/** PBKDF2-HMAC-SHA1 (RFC 8018) producing @p out_len bytes. */
void gw_pbkdf2_sha1(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len,
                    uint32_t iterations, uint8_t *out, size_t out_len);

/**
 * The PMK of @p passphrase for @p ssid. False (and @p out cleared) when the passphrase does not
 * obey the rule or the SSID is empty or longer than 32 bytes.
 */
bool gw_wpa_pmk(const char *passphrase, const char *ssid, uint8_t out[GW_WPA_PMK_LEN]);

/** gw_wpa_pmk() as 64 lower-case hexadecimal characters and a NUL ("" on failure). */
bool gw_wpa_pmk_hex(const char *passphrase, const char *ssid, char out[GW_WPA_PMK_HEX_SIZE]);

/** True when @p s is exactly 64 hexadecimal characters (a stored PMK). */
bool gw_wpa_pmk_hex_valid(const char *s);

/** The SSID of the access point: "GW-Setup-" and the last 3 bytes of @p mac in hexadecimal. */
void gw_wpa_ssid(const uint8_t mac[6], char out[GW_WPA_SSID_SIZE]);

#ifdef __cplusplus
}
#endif
