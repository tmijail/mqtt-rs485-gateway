#include "gw_debounce.h"

#include <string.h>

void gw_debounce_init(gw_debounce_t *db, uint16_t threshold, uint8_t initial)
{
    memset(db, 0, sizeof(*db));
    db->stable = initial;
    for (int i = 0; i < 8; i++) {
        db->threshold[i] = threshold == 0 ? 1 : threshold;
    }
}

void gw_debounce_init_each(gw_debounce_t *db, const uint16_t threshold[8], uint8_t invert,
                           uint8_t raw)
{
    memset(db, 0, sizeof(*db));
    db->invert = invert;
    db->stable = (uint8_t)(raw ^ invert);
    for (int i = 0; i < 8; i++) {
        db->threshold[i] = threshold[i] == 0 ? 1 : threshold[i];
    }
}

uint16_t gw_debounce_samples(uint16_t ms, uint32_t sample_us)
{
    uint32_t n = sample_us != 0 ? (uint32_t)ms * 1000u / sample_us : 0;
    return (uint16_t)(n == 0 ? 1 : n > UINT16_MAX ? UINT16_MAX : n);
}

uint8_t gw_debounce_sample(gw_debounce_t *db, uint8_t raw)
{
    uint8_t changed = 0;
    uint8_t diff = (uint8_t)((raw ^ db->invert) ^ db->stable);
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(diff & bit)) {
            db->count[i] = 0;
        } else if (++db->count[i] >= db->threshold[i]) {
            db->count[i] = 0;
            db->stable ^= bit;
            changed |= bit;
        }
    }
    return changed;
}
