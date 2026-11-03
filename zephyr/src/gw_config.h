/*
 * Gateway configuration model (common JSON schema, SPEC section 6).
 *
 * All storage is static and bounded by Kconfig limits (REL-2). The parsed
 * configuration is immutable once the gateway is running.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GW_CONFIG_H_
#define GW_CONFIG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_NAME_MAX      32  /* Sparkplug ids, device and tag names */
#define GW_HOST_MAX      63  /* host names, NTP server */
#define GW_SECRET_MAX    63  /* MQTT user name / password / client id */
#define GW_CFG_ERR_MAX   160 /* size of the error message buffer */

enum gw_area {
	GW_AREA_COIL,
	GW_AREA_DISCRETE,
	GW_AREA_HOLDING,
	GW_AREA_INPUT,
};

enum gw_type {
	GW_TYPE_BOOL,
	GW_TYPE_UINT16,
	GW_TYPE_INT16,
	GW_TYPE_UINT32,
	GW_TYPE_INT32,
	GW_TYPE_FLOAT32,
};

enum gw_word_order {
	GW_WORD_BIG,    /* ABCD: high word first */
	GW_WORD_LITTLE, /* CDAB: low word first */
};

enum gw_parity {
	GW_PARITY_NONE,
	GW_PARITY_EVEN,
	GW_PARITY_ODD,
};

struct gw_tag_cfg {
	char name[GW_NAME_MAX + 1];
	uint16_t address;
	uint8_t area;       /* enum gw_area */
	uint8_t type;       /* enum gw_type */
	uint8_t word_order; /* enum gw_word_order */
	bool writable;
	double scale;
	double offset;
	double deadband;
};

struct gw_device_cfg {
	char name[GW_NAME_MAX + 1];
	uint8_t unit_id;
	uint16_t offline_after;
	uint32_t poll_ms;
	uint16_t first_tag; /* index into gw_config.tags */
	uint16_t tag_count;
};

struct gw_config {
	struct {
		bool dhcp;
		uint8_t ip[4];
		uint8_t netmask[4];
		uint8_t gateway[4];
		uint8_t dns[4];
		bool has_dns;
	} network;
	struct {
		char server[GW_HOST_MAX + 1]; /* empty: SNTP disabled */
	} ntp;
	struct {
		char host[GW_HOST_MAX + 1];
		uint16_t port;
		char client_id[GW_SECRET_MAX + 1]; /* defaulted to edge_node_id */
		char username[GW_SECRET_MAX + 1];
		char password[GW_SECRET_MAX + 1];
		uint16_t keepalive_s;
	} mqtt;
	struct {
		char group_id[GW_NAME_MAX + 1];
		char edge_node_id[GW_NAME_MAX + 1];
		char primary_host_id[GW_NAME_MAX + 1];
	} sparkplug;
	struct {
		uint32_t baud;
		uint8_t parity; /* enum gw_parity */
		uint8_t stop_bits;
		uint16_t response_timeout_ms;
		uint8_t retries;
		uint16_t inter_frame_delay_ms;
	} rs485;
	struct {
		uint16_t di_debounce_ms;
	} local_io;

	uint8_t device_count;
	uint16_t tag_count;
	struct gw_device_cfg devices[CONFIG_GW_MAX_DEVICES];
	struct gw_tag_cfg tags[CONFIG_GW_MAX_TAGS];
};

/**
 * @brief Parse and validate a JSON configuration document.
 *
 * The document is not modified. On failure @p cfg is left in an unspecified
 * state and @p err holds a human readable message that names the offending
 * field (for example "devices[0].tags[2].type: unknown type 'u16'") and,
 * for syntax errors, the line and column.
 *
 * @retval 0 on success, -EINVAL on any syntax or validation error.
 */
int gw_config_parse(const char *json, size_t len, struct gw_config *cfg, char *err,
		    size_t err_size);

/** Number of 16-bit registers used by a tag type (1 for bit areas). */
static inline uint8_t gw_type_regs(enum gw_type type)
{
	return (type == GW_TYPE_UINT32 || type == GW_TYPE_INT32 || type == GW_TYPE_FLOAT32) ? 2
											  : 1;
}

static inline bool gw_area_is_bit(enum gw_area area)
{
	return area == GW_AREA_COIL || area == GW_AREA_DISCRETE;
}

#endif /* GW_CONFIG_H_ */
