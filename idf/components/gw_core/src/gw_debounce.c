#include "gw_debounce.h"

#include <string.h>

void gw_debounce_init(gw_debounce_t *db, uint16_t threshold, uint8_t initial)
{
    memset(db, 0, sizeof(*db));
    db->stable = initial;
    db->threshold = threshold == 0 ? 1 : threshold;
}

uint8_t gw_debounce_sample(gw_debounce_t *db, uint8_t raw)
{
    uint8_t changed = 0;
    uint8_t diff = raw ^ db->stable;
    for (int i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(diff & bit)) {
            db->count[i] = 0;
        } else if (++db->count[i] >= db->threshold) {
            db->count[i] = 0;
            db->stable ^= bit;
            changed |= bit;
        }
    }
    return changed;
}
