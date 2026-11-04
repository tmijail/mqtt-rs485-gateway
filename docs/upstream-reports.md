# Reportes para proyectos de terceros (pendientes de enviar)

Defectos de componentes de Espressif encontrados al desarrollar el firmware. Desde el entorno de desarrollo no se pueden abrir issues en esos repositorios: el texto está listo para que el mantenedor lo envíe. Cada uno dice dónde está el arreglo provisorio en este repositorio, para quitarlo cuando haya una versión corregida.

## 1. espressif/esp-mqtt: `esp_mqtt_set_config()` pierde memoria con `network.if_name` y `key_password`

- **Repositorio:** https://github.com/espressif/esp-mqtt (componente `espressif/mqtt`).
- **Versión:** 1.1.0 (la fijada en `idf/components/gw_edge/idf_component.yml`).
- **Arreglo provisorio aquí:** `set_config()` en `idf/components/gw_edge/gw_edge.c` y el test `check_set_config` (issue #58).

Texto propuesto (en inglés):

> **Title:** esp_mqtt_set_config() leaks network.if_name and authentication.key_password on every call
>
> **Version:** espressif/mqtt 1.1.0 (IDF v6.1)
>
> `esp_mqtt_set_config()` allocates a new copy of `config->network.if_name` (mqtt_client.c, around line 633: `client->config->if_name = calloc(1, sizeof(struct ifreq) + 1)`) without freeing the previous `client->config->if_name`. The same happens with `credentials.authentication.key_password` (`client->config->clientkey_password = malloc(...)`, around line 683). Every call with either field set leaks one allocation (about 17 bytes for `if_name`).
>
> Applications that call `esp_mqtt_set_config()` before every reconnection, for example to refresh the last will (Sparkplug B puts a new bdSeq in the will of every connection attempt), leak continuously.
>
> **Steps:** create a client with `network.if_name` set, then call `esp_mqtt_set_config()` with the same configuration in a loop; the free heap goes down by the size of `struct ifreq` + 1 per call.
>
> **Expected:** free the previous copy before allocating the new one (as done for the host, the client id and the will message), or keep the existing allocation.

## 2. ESP-IDF esp-tls (mbedTLS): con TLS 1.3 no se informa por qué falló la verificación del certificado

- **Repositorio:** https://github.com/espressif/esp-idf, componente `esp-tls` (`components/esp-tls/esp_tls_mbedtls.c`).
- **Versión:** ESP-IDF v6.1, mbedTLS 4.1.
- **Arreglo provisorio aquí:** `idf/components/gw_edge/gw_edge.c` registra los *flags* con su propio callback de verificación de mbedTLS (issue #70).

Texto propuesto (en inglés):

> **Title:** esp-tls does not report the certificate verification flags when a TLS 1.3 handshake fails
>
> **Version:** ESP-IDF v6.1 (mbedTLS 4.1), client side
>
> When the server certificate fails verification, `esp_mbedtls_handshake()` calls `esp_mbedtls_verify_certificate()` only if `mbedtls_ssl_get_peer_cert()` returns a certificate (esp_tls_mbedtls.c, around line 291). With TLS 1.3 the peer certificate is no longer available after the failed handshake, so `ESP_TLS_ERR_TYPE_MBEDTLS_CERT_FLAGS` is never captured: `esp_tls_get_and_clear_last_error()` (and esp-mqtt's `esp_tls_cert_verify_flags`) report 0, and the application only sees `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED` (-0x2700) without knowing whether the CA, the name or the validity dates were wrong. (Observed with TLS 1.3; the code path does not depend on the version, so TLS 1.2 is likely affected whenever the peer certificate is freed on failure.)
>
> **Steps:** connect with esp-tls (for example through esp-mqtt) to a TLS 1.3-only server whose certificate is expired, or carries another name; read the error handle after the failure.
>
> **Expected:** the verification flags of the failed handshake, for example by calling `mbedtls_ssl_get_verify_result()` regardless of the peer certificate, or by recording them in a verify callback set by esp-tls.
