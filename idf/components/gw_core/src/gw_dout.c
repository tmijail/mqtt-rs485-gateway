#include "gw_dout.h"

#include <stddef.h>

static int start(gw_dout_t *d)
{
    uint8_t off = gw_active_to_pins(0, d->active_low);
    d->attempts++;
    int err = 0;
    uint8_t latch = off;
    if (d->hold != 0) {
        /* IO-3: a held output keeps its latch level only if its pin already is an output */
        uint8_t config = 0xFF, out = off;
        err = d->bus.read_reg(d->bus.ctx, GW_TCA9554_REG_CONFIG, &config);
        if (err == 0) {
            err = d->bus.read_reg(d->bus.ctx, GW_TCA9554_REG_OUTPUT, &out);
        }
        uint8_t keep = (uint8_t)(d->hold & ~config);
        latch = (uint8_t)((out & keep) | (off & ~keep));
    }
    d->latch = err == 0 ? latch : off;
    if (err == 0) {
        err = d->bus.write_reg(d->bus.ctx, GW_TCA9554_REG_OUTPUT, d->latch);
    }
    if (err == 0) {
        err = d->bus.write_reg(d->bus.ctx, GW_TCA9554_REG_POLARITY, 0x00);
    }
    if (err == 0) {
        err = d->bus.write_reg(d->bus.ctx, GW_TCA9554_REG_CONFIG, 0x00);
    }
    d->ready = err == 0;
    return err;
}

int gw_dout_init(gw_dout_t *d, const gw_dout_bus_t *bus, uint8_t active_low, uint8_t hold)
{
    d->bus = *bus;
    d->active_low = active_low;
    d->hold = hold;
    d->ready = false;
    d->attempts = 0;
    d->retry_delay_ms = 0;
    d->retry_at_ms = 0;
    return start(d);
}

bool gw_dout_recover(gw_dout_t *d, uint32_t now_ms)
{
    if (d->ready || d->bus.write_reg == NULL) {
        return false;
    }
    if (d->retry_delay_ms == 0) {
        d->retry_delay_ms = GW_DOUT_RETRY_MIN_MS;
        d->retry_at_ms = now_ms + d->retry_delay_ms;
        return false;
    }
    if ((int32_t)(now_ms - d->retry_at_ms) < 0) {
        return false;
    }
    if (start(d) == 0) {
        return true;
    }
    d->retry_delay_ms = d->retry_delay_ms >= GW_DOUT_RETRY_MAX_MS / 2 ? GW_DOUT_RETRY_MAX_MS
                                                                      : d->retry_delay_ms * 2;
    d->retry_at_ms = now_ms + d->retry_delay_ms;
    return false;
}

int gw_dout_set(gw_dout_t *d, uint8_t index, bool on)
{
    uint8_t bit = (uint8_t)(1u << index);
    return gw_dout_write(d, bit, on ? bit : 0);
}

uint8_t gw_dout_state(const gw_dout_t *d)
{
    return gw_pins_to_active(d->latch, d->active_low);
}

int gw_dout_write(gw_dout_t *d, uint8_t mask, uint8_t active)
{
    uint8_t now = gw_pins_to_active(d->latch, d->active_low);
    now = (uint8_t)((now & ~mask) | (active & mask));
    uint8_t latch = gw_active_to_pins(now, d->active_low);
    int err = d->bus.write_reg(d->bus.ctx, GW_TCA9554_REG_OUTPUT, latch);
    if (err == 0) {
        d->latch = latch;
    }
    return err;
}

uint8_t gw_dout_read(const gw_dout_t *d, bool *from_pins)
{
    uint8_t pins;
    *from_pins = d->bus.read_reg(d->bus.ctx, GW_TCA9554_REG_INPUT, &pins) == 0;
    return gw_pins_to_active(*from_pins ? pins : d->latch, d->active_low);
}
