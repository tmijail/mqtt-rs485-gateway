/*
 * Modbus RTU master transaction engine: inter-frame silence, response timeout,
 * retries and error counters, on top of an abstract byte transport.
 *
 * One mb_master_t must be used by a single task: it is the RS485 bus owner (MB-16).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mb_rtu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Transmit @p len bytes and return only once the last stop bit left the wire. 0 = OK. */
    int (*write)(void *ctx, const uint8_t *data, size_t len);
    /** Read up to @p len bytes, waiting at most @p timeout_ms for all of them. Returns count. */
    int (*read)(void *ctx, uint8_t *data, size_t len, uint32_t timeout_ms);
    /** Discard any pending received bytes. */
    void (*flush)(void *ctx);
    /** Busy-wait or sleep for @p us microseconds. */
    void (*delay_us)(void *ctx, uint32_t us);
    /** Monotonic microsecond clock. */
    uint64_t (*now_us)(void *ctx);
    /** Called before every attempt; the target uses it to feed the task watchdog. May be NULL. */
    void (*on_attempt)(void *ctx);
    void *ctx;
} mb_transport_t;

typedef struct {
    uint32_t baud;
    uint16_t response_timeout_ms;
    uint8_t retries;                /* extra attempts after the first one */
    uint16_t inter_frame_delay_ms;  /* extra delay between transactions, on top of t3.5 */
} mb_master_cfg_t;

typedef struct {
    uint32_t ok;
    uint32_t timeout;
    uint32_t crc;
    uint32_t frame;
    uint32_t exception;
} mb_stats_t;

typedef struct {
    mb_transport_t tr;
    mb_master_cfg_t cfg;
    uint32_t char_us;      /* duration of one 11-bit character */
    uint32_t t35_us;       /* 3.5 characters, 1750 us above 19200 baud */
    uint64_t last_bus_us;  /* end of the last frame seen on the bus */
    mb_stats_t stats;
    uint8_t tx[MB_ADU_MAX];
    uint8_t rx[MB_ADU_MAX];
} mb_master_t;

void mb_master_init(mb_master_t *m, const mb_transport_t *tr, const mb_master_cfg_t *cfg);

/** Bits on the wire per character: start + 8 data + parity (if any) + 1 or 2 stop bits. */
uint32_t mb_char_bits(bool parity, uint8_t stop_bits);

/**
 * Upper bound, in ms, for shifting @p len bytes out at @p baud with @p char_bits bits per
 * character, plus a fixed margin for scheduler latency. The transport waits at most this
 * long for the end of a transmission, so the bound grows with the frame: a fixed limit
 * cut long frames at low baud rates (a 13-byte FC16 takes 119 ms at 1200 baud with parity).
 */
uint32_t mb_tx_timeout_ms(uint32_t baud, uint32_t char_bits, size_t len);

/**
 * Execute one transaction, retrying on timeout, CRC and framing errors.
 * Exception responses are final (not retried). On MB_OK / MB_ERR_EXCEPTION
 * @p rsp is filled; its data pointer stays valid until the next call.
 */
mb_status_t mb_master_execute(mb_master_t *m, const mb_request_t *rq, mb_response_t *rsp);

#ifdef __cplusplus
}
#endif
