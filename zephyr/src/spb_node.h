/*
 * Sparkplug B 3.0 edge node semantics (SPB-2..SPB-12).
 *
 * Transport agnostic: the MQTT session (mqtt_link.c) owns the connection
 * and calls into this module from its single thread; publishing and the
 * side effects of commands go through @ref spb_node_ops so the logic can
 * be unit tested on the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_SPB_NODE_H_
#define GW_SPB_NODE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_config.h"

/* Alias plan (SPB-8): node metrics use small fixed aliases, device tag n
 * (global index into gw_config.tags) uses SPB_ALIAS_TAG_BASE + n.
 */
#define SPB_ALIAS_BDSEQ     1
#define SPB_ALIAS_REBIRTH   2
#define SPB_ALIAS_REBOOT    3
#define SPB_ALIAS_FIRMWARE  4
#define SPB_ALIAS_HARDWARE  5
#define SPB_ALIAS_DI_BASE   11 /* DI/1 .. DI/8 -> 11..18 */
#define SPB_ALIAS_DO_BASE   21 /* DO/1 .. DO/8 -> 21..28 */
#define SPB_ALIAS_TAG_BASE  1000

struct spb_node_ops {
	/** Publish one message (QoS 0 or 1, never retained). */
	int (*publish)(const char *topic, const uint8_t *payload, size_t len, uint8_t qos);
	/** Queue a write of an engineering value to a Modbus tag (DCMD). */
	int (*write_tag)(uint8_t dev, uint16_t tag, double value);
	/** Drive local output @p channel (0-based); the model is updated by the I/O module. */
	int (*set_output)(uint8_t channel, bool on);
	/** Wall clock in ms since the Unix epoch (UTC). */
	int64_t (*now_ms)(void);
};

enum spb_node_request {
	SPB_NODE_REQ_NONE = 0,
	SPB_NODE_REQ_REBOOT = 1, /* NCMD Node Control/Reboot received */
};

void spb_node_init(const struct gw_config *cfg, const struct spb_node_ops *ops,
		   const char *firmware, const char *hardware);

/**
 * Encode the NDEATH payload for @p bdseq (used as the MQTT will and for an
 * orderly disconnect). The buffer must outlive the MQTT connection.
 */
int spb_node_encode_death(uint8_t bdseq, uint8_t *buf, size_t size, size_t *len);

/** A new MQTT session is up: schedule NBIRTH (seq=0) and all DBIRTHs. */
void spb_node_session_start(uint8_t bdseq);

/** The MQTT session is gone: every device must be born again next time. */
void spb_node_session_end(void);

/**
 * Publish whatever the current state requires: pending BIRTHs, DDEATH for
 * devices that went offline and DATA messages with the changed metrics.
 *
 * @retval <0 if a publish failed (the session should be dropped), otherwise
 *         an @ref spb_node_request the session layer must act upon.
 */
int spb_node_service(void);

/** Handle an incoming PUBLISH on one of the subscribed command topics. */
void spb_node_on_command(const char *topic, size_t topic_len, const uint8_t *payload,
			 size_t len);

/** Current value of the shared message sequence number (for diagnostics). */
uint8_t spb_node_seq(void);

#endif /* GW_SPB_NODE_H_ */
