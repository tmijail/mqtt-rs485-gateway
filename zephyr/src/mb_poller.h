/*
 * Modbus RTU master: the single owner of the RS485 bus (MB-16).
 *
 * One thread performs every transaction on the bus: periodic polling of
 * each device (MB-10) and the DCMD writes it receives through a bounded
 * queue (MB-4, SPB-10). Writes are served before the next read.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_MB_POLLER_H_
#define GW_MB_POLLER_H_

#include <stdint.h>

#include "gw_config.h"

/** Configure the Modbus client interface and start the bus owner thread. */
int mb_poller_start(const struct gw_config *cfg);

/**
 * Queue a write of an engineering value to a writable tag. The confirmed
 * value is reported through the model once the slave acknowledged it.
 *
 * @retval 0 queued, -ENOBUFS queue full, -ENODEV poller not running.
 */
int mb_poller_write(uint8_t dev, uint16_t tag, double value);

#endif /* GW_MB_POLLER_H_ */
