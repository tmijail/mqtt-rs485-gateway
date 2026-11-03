/* RS485 port: UART in hardware half-duplex mode with RTS as driver enable (MB-1, MB-2),
 * exposed as an mb_transport_t for the Modbus master. */
#pragma once

#include "esp_err.h"
#include "gw_config.h"
#include "mb_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the UART from the "rs485" section and fill @p tr. Call once. */
esp_err_t gw_rs485_init(const gw_config_t *cfg, mb_transport_t *tr);

#ifdef __cplusplus
}
#endif
