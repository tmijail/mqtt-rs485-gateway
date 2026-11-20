/*
 * Issue #274 (NET-5): the DNS server of the captive portal, private to gw_ap. It runs only while
 * the access point runs: gw_ap starts it on the interface of the access point and stops it with
 * the access point. It answers each A query of a client of that interface with the address of the
 * interface (gw_captive_dns_answer); see gw_apdns.c for where it runs and its memory.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Start (or move) the server on the lwIP interface with index @p netif_index (an index of
 *  esp_netif_get_netif_impl_index). From any task but the tcpip task. */
esp_err_t gw_apdns_start(int netif_index);

/** Stop the server, if it runs. From any task but the tcpip task. */
void gw_apdns_stop(void);

/** The server runs. */
bool gw_apdns_running(void);

/** Answers sent, and queries without an answer, since boot. */
void gw_apdns_counts(uint32_t *answered, uint32_t *dropped);
