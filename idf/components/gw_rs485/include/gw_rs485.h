/* RS485 port: UART in hardware half-duplex mode with RTS as driver enable (MB-1, MB-2),
 * exposed as an mb_transport_t for the Modbus master. */
#pragma once

#include "esp_err.h"
#include "gw_config.h"
#include "mb_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Issue #287: make the DE pin of the transceiver (gw_bsp.rs485.de) a GPIO output at its
 * inactive level, so that the transceiver does not drive the bus. Call it first in app_main, in
 * every mode: an input DE pin lets the isolator of the board give a high DE, and the transmitter
 * then holds the bus. It does not touch the UART. gw_rs485_init() later gives the pin to the
 * UART as RTS without a pulse on DE.
 */
esp_err_t gw_rs485_de_off(void);

/** Configure the UART from the "rs485" section and fill @p tr. Call once. */
esp_err_t gw_rs485_init(const gw_config_t *cfg, mb_transport_t *tr);

#ifdef __cplusplus
}
#endif
