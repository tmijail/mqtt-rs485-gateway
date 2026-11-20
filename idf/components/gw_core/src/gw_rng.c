/* Issue #267: the entropy sources of the RNG and the order of the boot (gw_rng.h). */
#include "gw_rng.h"

#include <stdatomic.h>

#include "gw_port.h"

static const char *TAG = "rng";

static atomic_bool s_sar;       /* the SAR ADC source is on */
static atomic_bool s_radio;     /* the radio runs */
static atomic_bool s_closed;    /* the radio, the ADC or the sensor started: no SAR ADC source */

static void sar_off(const char *why)
{
    if (atomic_exchange(&s_sar, false)) {
        gw_port_rng_sar(false);
        GW_LOGI(TAG, "SAR ADC entropy source off: %s", why);
    }
}

void gw_rng_boot(void)
{
    if (atomic_load(&s_closed)) {
        GW_LOGE(TAG, "SAR ADC entropy source not turned on: the radio, the ADC or the temperature "
                     "sensor started");
        return;
    }
    if (!atomic_exchange(&s_sar, true)) {
        gw_port_rng_sar(true);
        GW_LOGI(TAG, "SAR ADC entropy source on (issue #267)");
    }
}

void gw_rng_radio_starts(void)
{
    atomic_store(&s_closed, true);
    sar_off("the Wi-Fi driver starts");
}

void gw_rng_radio_on(bool on)
{
    atomic_store(&s_radio, on);
    if (on) {
        GW_LOGI(TAG, "entropy source: the radio (station, no connection, no power save), until "
                     "the restart");
    } else {
        GW_LOGE(TAG, "the radio did not start: the random values have no entropy source with a "
                     "guarantee (only the internal oscillator)");
    }
}

void gw_rng_adc_starts(const char *who)
{
    atomic_store(&s_closed, true);
    sar_off(who);
}

gw_rng_src_t gw_rng_source(void)
{
    return atomic_load(&s_radio) ? GW_RNG_SRC_RADIO
           : atomic_load(&s_sar) ? GW_RNG_SRC_SAR_ADC
                                 : GW_RNG_SRC_NONE;
}

const char *gw_rng_source_name(gw_rng_src_t src)
{
    return src == GW_RNG_SRC_RADIO ? "radio" : src == GW_RNG_SRC_SAR_ADC ? "SAR ADC" : "none";
}

gw_rng_src_t gw_rng_note(const char *what)
{
    gw_rng_src_t src = gw_rng_source();
    if (src == GW_RNG_SRC_NONE) {
        GW_LOGW(TAG, "%s: made with no entropy source with a guarantee (only the internal "
                     "oscillator)", what);
    } else {
        GW_LOGI(TAG, "%s: made with the %s entropy source", what, gw_rng_source_name(src));
    }
    return src;
}

void gw_rng_reset(void)
{
    if (atomic_exchange(&s_sar, false)) {
        gw_port_rng_sar(false);
    }
    atomic_store(&s_radio, false);
    atomic_store(&s_closed, false);
}
