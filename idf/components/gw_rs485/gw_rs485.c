#include "gw_rs485.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "gw_bsp.h"

static const char *TAG = "rs485";

#include "mb_master.h"

#define PORT (gw_bsp.rs485.uart)
#define RX_BUF_SIZE 512          /* > MB_ADU_MAX; the driver needs > SOC_UART_FIFO_LEN */
#define RX_TOUT_SYMBOLS 3        /* hand bytes to the ring buffer after ~3 idle characters */

#if CONFIG_GW_RS485_HW_HALF_DUPLEX
#define MODE_NOTE ", hardware half-duplex"
#else
#define MODE_NOTE ", plain UART (RTS not driven)"
#endif

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
    ESP_RETURN_ON_ERROR(uart_set_pin(PORT, gw_bsp.rs485.tx, gw_bsp.rs485.rx, gw_bsp.rs485.de,
                                     UART_PIN_NO_CHANGE),
                        TAG, "pins");
    /* In RS485 half-duplex mode the driver raises RTS while sending: DE active high. */
    if (gw_bsp.rs485.de_active_low) {
        ESP_RETURN_ON_ERROR(uart_set_line_inverse(PORT, UART_SIGNAL_RTS_INV), TAG, "DE polarity");
    }
#if CONFIG_GW_RS485_HW_HALF_DUPLEX
    ESP_RETURN_ON_ERROR(uart_set_mode(PORT, UART_MODE_RS485_HALF_DUPLEX), TAG, "rs485 mode");
#endif
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(PORT, RX_TOUT_SYMBOLS), TAG, "rx timeout");

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
