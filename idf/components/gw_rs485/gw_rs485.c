#include "gw_rs485.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gw_bsp.h"
#include "hal/uart_ll.h"

static const char *TAG = "rs485";

#include "mb_master.h"

#define PORT (gw_bsp.rs485.uart)
#define RX_BUF_SIZE 512          /* > MB_ADU_MAX; the driver needs > SOC_UART_FIFO_LEN */
#define RX_TOUT_SYMBOLS 3        /* hand bytes to the ring buffer after ~3 idle characters */

#if CONFIG_GW_RS485_HW_HALF_DUPLEX
#define MODE_NOTE ", hardware half-duplex"
#else
#define MODE_NOTE ", plain UART (DE always inactive)"
#endif

/* The inactive level of the DE pin: the transceiver does not drive the bus. */
#define DE_OFF_LEVEL (gw_bsp.rs485.de_active_low ? 1U : 0U)

static uint32_t s_baud;
static uint32_t s_char_bits;

static TickType_t ms_to_ticks(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    return t == 0 ? 1 : t;
}

static int tr_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (uart_write_bytes(PORT, data, len) != (int)len) {
        return -1;
    }
    /* With no TX ring buffer uart_write_bytes() returns once the frame is in the FIFO, so the
     * wait covers the whole frame: its timeout grows with the length and the character time
     * (an FC16 of 13 bytes takes 119 ms at 1200 baud with parity). Returns once the shift
     * register is empty; RTS is released by the driver on TX_DONE. */
    uint32_t ms = mb_tx_timeout_ms(s_baud, s_char_bits, len);
    return uart_wait_tx_done(PORT, ms_to_ticks(ms) + 1) == ESP_OK ? 0 : -1;
}

static int tr_read(void *ctx, uint8_t *data, size_t len, uint32_t timeout_ms)
{
    (void)ctx;
    return uart_read_bytes(PORT, data, len, ms_to_ticks(timeout_ms));
}

static void tr_flush(void *ctx)
{
    (void)ctx;
    uart_flush_input(PORT);
}

static void tr_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    if (us >= 2000) {
        vTaskDelay(ms_to_ticks(us / 1000));
        us %= 1000;
    }
    if (us > 0) {
        esp_rom_delay_us(us);
    }
}

static uint64_t tr_now_us(void *ctx)
{
    (void)ctx;
    return (uint64_t)esp_timer_get_time();
}

static void tr_on_attempt(void *ctx)
{
    (void)ctx;
    esp_task_wdt_reset();  /* the calling task (bus owner) is subscribed to the TWDT */
}

/* QEMU keeps UART1's interrupt line across a restart (IDF resets UART0 on the way out, not
 * UART1, and the chip clears it in uart_driver_install(); QEMU's model does not): with an RX
 * interrupt pending at that moment (a slave answering while the gateway restarts), the next
 * boot's uart_driver_install() unmasks a line that no register write lowers any more, and CPU0
 * never leaves the interrupt dispatcher. So the port's interrupts are masked before every
 * restart:
 *  - esp_restart(): from a shutdown handler, through the driver (so it does not re-enable the
 *    RX interrupts afterwards) and with interrupts still enabled;
 *  - every restart, panics included (the panic handler runs with interrupts off and calls no
 *    shutdown handler): esp_restart_noos() is wrapped (-Wl,--wrap, CMakeLists.txt) to mask
 *    them and also reset the RX FIFO, which drops the RX condition itself.
 * 40 restarts in a row in QEMU (20 NCMD Reboots, 20 panics), no hang; before, a hang within the
 * first 1 to 7. Harmless on the chip, where the driver resets the UART at the next boot. */
void __real_esp_restart_noos(void) __attribute__((noreturn));
void __wrap_esp_restart_noos(void) __attribute__((noreturn));

static uart_port_t s_port = UART_NUM_MAX;   /* set once the driver is installed */

static void IRAM_ATTR mask_port(void)
{
    if (s_port < UART_NUM_MAX) {   /* driver installed: the UART is clocked */
        uart_dev_t *hw = UART_LL_GET_HW(s_port);
        uart_ll_disable_intr_mask(hw, UINT32_MAX);
        uart_ll_rxfifo_rst(hw);   /* drop the RX condition itself, not only its mask */
        uart_ll_clr_intsts_mask(hw, UINT32_MAX);
    }
}

void IRAM_ATTR __wrap_esp_restart_noos(void)
{
    mask_port();
    __real_esp_restart_noos();
}

static void mask_on_restart(void)
{
    /* the driver's own call: it also stops the driver from re-enabling the RX interrupts */
    uart_disable_intr_mask(s_port, UINT32_MAX);
    uart_clear_intr_status(s_port, UINT32_MAX);
}

/* Issue #287: on the board, DE of the SP3485E comes from the DE pin through an isolator whose
 * open input gives a HIGH output. While the pin is an input (the reset, the ROM, the bootloader,
 * the start of the application, and each mode that does not start gw_bus), the transmitter is
 * on and holds the bus. The level is written first, so the pad becomes an output already at
 * the inactive level. Not gpio_config(): it reserves the pin, and uart_set_pin() then logs a
 * conflict for the RTS pin. */
esp_err_t gw_rs485_de_off(void)
{
    const gpio_num_t de = gw_bsp.rs485.de;
    ESP_RETURN_ON_ERROR(gpio_set_level(de, DE_OFF_LEVEL), TAG, "DE level");
    ESP_RETURN_ON_ERROR(gpio_output_enable(de), TAG, "DE output");
    ESP_LOGI(TAG, "DE GPIO%d %s: the transceiver does not drive the bus", de,
             DE_OFF_LEVEL ? "high" : "low");
    return ESP_OK;
}

esp_err_t gw_rs485_init(const gw_config_t *cfg, mb_transport_t *tr)
{
    static const uart_parity_t parity[] = {UART_PARITY_DISABLE, UART_PARITY_EVEN, UART_PARITY_ODD};
    const uart_config_t uc = {
        .baud_rate = (int)cfg->rs485.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = parity[cfg->rs485.parity],
        .stop_bits = cfg->rs485.stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    s_baud = cfg->rs485.baud;
    s_char_bits = mb_char_bits(cfg->rs485.parity != GW_PARITY_NONE, cfg->rs485.stop_bits);
#if CONFIG_UART_ISR_IN_IRAM
    const int intr_flags = ESP_INTR_FLAG_IRAM;   /* DE is released in the ISR (see sdkconfig) */
#else
    const int intr_flags = 0;
#endif
    ESP_RETURN_ON_ERROR(uart_driver_install(PORT, RX_BUF_SIZE, 0, 0, NULL, intr_flags), TAG,
                        "install");
    ESP_RETURN_ON_ERROR(uart_param_config(PORT, &uc), TAG, "param");
    /* Issue #287: the RTS signal gets its idle level before the DE pin goes to the UART, so
     * the change from the GPIO (gw_rs485_de_off) to RTS gives no pulse on DE. After the reset
     * of the UART the software RTS (SW_RTS = 0) gives a HIGH pin. SW_RTS = 1 gives a LOW pin, and
     * the RS485 half-duplex mode keeps it so between two frames. In this mode the UART raises
     * RTS while it sends: DE active high. RTS_INV gives the levels of an active-low DE. */
    if (gw_bsp.rs485.de_active_low) {
        ESP_RETURN_ON_ERROR(uart_set_line_inverse(PORT, UART_SIGNAL_RTS_INV), TAG, "DE polarity");
    }
    ESP_RETURN_ON_ERROR(uart_set_rts(PORT, 1), TAG, "DE idle");
#if CONFIG_GW_RS485_HW_HALF_DUPLEX
    ESP_RETURN_ON_ERROR(uart_set_mode(PORT, UART_MODE_RS485_HALF_DUPLEX), TAG, "rs485 mode");
#endif
    ESP_RETURN_ON_ERROR(uart_set_pin(PORT, gw_bsp.rs485.tx, gw_bsp.rs485.rx, gw_bsp.rs485.de,
                                     UART_PIN_NO_CHANGE),
                        TAG, "pins");
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(PORT, RX_TOUT_SYMBOLS), TAG, "rx timeout");
    s_port = PORT;
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(mask_on_restart), TAG, "shutdown handler");

    *tr = (mb_transport_t){
        .write = tr_write,
        .read = tr_read,
        .flush = tr_flush,
        .delay_us = tr_delay_us,
        .now_us = tr_now_us,
        .on_attempt = tr_on_attempt,
        .ctx = NULL,
    };
    ESP_LOGI(TAG, "UART%d %lu baud, parity %d, %u stop bit(s), TX %d RX %d DE %d (active %s)" MODE_NOTE,
             PORT, (unsigned long)cfg->rs485.baud, cfg->rs485.parity, cfg->rs485.stop_bits,
             gw_bsp.rs485.tx, gw_bsp.rs485.rx, gw_bsp.rs485.de,
             gw_bsp.rs485.de_active_low ? "low" : "high");
    return ESP_OK;
}
