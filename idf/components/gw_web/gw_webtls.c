/*
 * Issue #291: TLS 1.2 clients of the HTTPS server with the dynamic TLS buffers of ESP-IDF
 * (CONFIG_MBEDTLS_DYNAMIC_BUFFER, #80).
 *
 * With TLS 1.2 and TLS 1.3 both on, the mbedTLS server reads each ClientHello in its TLS 1.3
 * code first. A ClientHello without TLS 1.3 in supported_versions goes to the TLS 1.2 code:
 * mbedTLS sets ssl->keep_current_message, and the TLS 1.2 state machine parses the same record
 * again from the RX buffer (ssl_tls13_process_client_hello, then ssl_parse_client_hello of
 * ssl_tls12_server.c). The dynamic-buffer layer of ESP-IDF v6.1
 * (components/mbedtls/port/dynamic/esp_ssl_srv.c, manage_resource) frees the RX buffer after
 * the CLIENT_HELLO step without a look at that flag (its client side, esp_ssl_cli.c, looks at
 * it). The ClientHello is lost, and the TLS 1.2 step then waits for a new record that never
 * comes: the client waits for the ServerHello. On the board, the server sent no byte until the
 * stall watchdog shut down the sessions ("Dynamic Impl: mbedtls_ssl_fetch_input error=76").
 *
 * The wrapper keeps the RX buffer while mbedTLS keeps a message in it. Only the dynamic-buffer
 * layer calls this function (--wrap in CMakeLists.txt). Its client side never calls it with the
 * flag set, so the wrapper changes nothing for the MQTT and OTA sessions. If a later ESP-IDF
 * looks at the flag itself, the wrapper has no effect; if it renames the function, the link
 * fails (no __real_ symbol).
 */
#include "sdkconfig.h"

#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
#include "mbedtls/ssl.h"

int __real_esp_mbedtls_free_rx_buffer(mbedtls_ssl_context *ssl);
int __wrap_esp_mbedtls_free_rx_buffer(mbedtls_ssl_context *ssl);

int __wrap_esp_mbedtls_free_rx_buffer(mbedtls_ssl_context *ssl)
{
    if (ssl->MBEDTLS_PRIVATE(keep_current_message)) {
        return 0;   /* the next step parses this message again: the record stays */
    }
    return __real_esp_mbedtls_free_rx_buffer(ssl);
}
#endif
