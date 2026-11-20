/*
 * Issue #267: the entropy sources of the random number generator (RNG) of the ESP32-S3, and the
 * order of the boot that keeps one of them on while the gateway makes a random value.
 *
 * ESP-IDF ("Random Number Generation") guarantees true random numbers from esp_random() and
 * esp_fill_random() only while one of these sources is on:
 *   - the SAR ADC source (bootloader_random_enable()). It must be off before the Wi-Fi driver,
 *     the ADC or the temperature sensor start (bootloader_random.h);
 *   - the RF subsystem: the Wi-Fi radio after esp_wifi_start(), not in a sleep mode.
 * The internal 8 MHz oscillator is always on, but has no guarantee alone. mbedTLS and PSA take each
 * random value from esp_fill_random() (MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG): no DRBG keeps a seed from
 * an earlier time, so the source at the time of the call is the source of the value.
 *
 * The sequence of the boot (main.c, gw_net.c, gw_temp.c):
 *   1. gw_storage_init(). With the NVS encryption (the secure QEMU build), nvs_sec_provider makes
 *      the HMAC key with its own bootloader_random_enable()/_disable() pair: gw_rng_boot() comes
 *      after it, so that this pair does not turn off the source of step 2.
 *   2. gw_rng_boot(): the SAR ADC source goes on. The commissioning password, and the salt of a
 *      web password in the configuration file (gw_storage_load_config), are made now.
 *   3. gw_net_start: gw_rng_radio_starts() turns the SAR ADC source off, just before the first
 *      esp_wifi_init(). The Wi-Fi driver then starts in station mode, with no connection, no
 *      access point, WIFI_PS_NONE and no modem sleep while not connected (sta_disconnected_pm)
 *      (gw_wifiown: GW_WIFI_USER_RNG), and gw_rng_radio_on(true).
 *      The radio stays on until the restart.
 *   4. gw_temp_start(): gw_rng_adc_starts() (the SAR ADC source is off already, or goes off now).
 *   5. The certificate of the web UI, the salts of later web passwords and the TLS sessions: the
 *      radio.
 * A build with no radio (QEMU, CONFIG_GW_WIFI=n) does steps 1 to 4 the same; after step 3 no source
 * with a guarantee is on. When the radio does not start, the same is true; the log gives it.
 *
 * The state is written by the boot (one task) and read by any task.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GW_RNG_SRC_NONE = 0,   /* only the internal oscillator: no guarantee */
    GW_RNG_SRC_SAR_ADC,    /* bootloader_random_enable() */
    GW_RNG_SRC_RADIO,      /* the RF subsystem: the Wi-Fi driver runs, WIFI_PS_NONE */
} gw_rng_src_t;

/** Step 2: the SAR ADC source on. Refused (an error in the log) after gw_rng_radio_starts() or
 *  gw_rng_adc_starts(): the source must not be on with the radio, the ADC or the sensor. */
void gw_rng_boot(void);

/** Step 3: the Wi-Fi driver starts now: the SAR ADC source goes off. */
void gw_rng_radio_starts(void);

/** Step 3: the radio runs (true), or did not start (false: an error in the log). */
void gw_rng_radio_on(bool on);

/** Step 4: the ADC or the temperature sensor (@p who) starts now: the SAR ADC source goes off. */
void gw_rng_adc_starts(const char *who);

/** The source with a guarantee that is on now. */
gw_rng_src_t gw_rng_source(void);

/** "SAR ADC", "radio" or "none". */
const char *gw_rng_source_name(gw_rng_src_t src);

/** The maker of a value that stays (@p what) calls this just before it makes the value: the log
 *  gives the source (info), or a warning when no source with a guarantee is on. */
gw_rng_src_t gw_rng_note(const char *what);

/** Host tests: the state of the boot again (the SAR ADC source off, no radio, no ADC). */
void gw_rng_reset(void);

#ifdef __cplusplus
}
#endif
