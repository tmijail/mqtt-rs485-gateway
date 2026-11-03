#include "gw_dout.h"

#include <stddef.h>

int gw_dout_init(gw_dout_t *d, const gw_dout_bus_t *bus, uint8_t active_low)
{
    d->bus = *bus;
    d->active_low = active_low;
    d->latch = gw_active_to_pins(0, active_low);
    d->ready = false;
    int err = bus->write_reg(bus->ctx, GW_TCA9554_REG_OUTPUT, d->latch);
    if (err == 0) {
        err = bus->write_reg(bus->ctx, GW_TCA9554_REG_CONFIG, 0x00);
    }
    d->ready = err == 0;
    return err;
}

int gw_dout_set(gw_dout_t *d, uint8_t index, bool on)
{
    uint8_t bit = (uint8_t)(1u << index);
    uint8_t active = gw_pins_to_active(d->latch, d->active_low);
    active = on ? (uint8_t)(active | bit) : (uint8_t)(active & ~bit);
    uint8_t latch = gw_active_to_pins(active, d->active_low);
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
