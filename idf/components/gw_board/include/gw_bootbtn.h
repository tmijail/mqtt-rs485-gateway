/* BOOT button (GPIO0) watcher: samples it every 20 ms in a small task and reports the
 * gestures of gw_button.h (CFG-5 factory reset after 10 s, NET-5 access point after a
 * 2-10 s press). LED feedback while held; two buzzer beeps at the reset (gw_buzzer). Issue
 * #187: the beep of the access point gesture comes from the handler, only when it accepts the
 * gesture. */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "gw_button.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @p on_event runs in the watcher task (low priority, not watched). */
esp_err_t gw_bootbtn_start(void (*on_event)(gw_btn_event_t ev));

#if CONFIG_GW_IO_SIMULATED
/** Emulator only: press or release the simulated BOOT button (console shell). */
void gw_bootbtn_sim_set(bool pressed);
#endif

#ifdef __cplusplus
}
#endif
