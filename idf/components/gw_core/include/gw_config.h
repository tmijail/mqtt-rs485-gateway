/*
 * Gateway configuration: the common JSON schema of SPEC section 6, parsed and
 * validated into a fixed-size structure (no pointers, no heap after parsing).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Static limits (SPEC section 6, "Limites del MVP"). */
#define GW_MAX_DEVICES          16
#define GW_MAX_TAGS_PER_DEVICE  64
#define GW_MAX_TAGS             256

/* Buffer sizes, including the terminating NUL. */
#define GW_NAME_SIZE   40   /* device and tag names */
#define GW_ID_SIZE     33   /* Sparkplug group_id / edge_node_id / primary_host_id */
#define GW_HOST_SIZE   128  /* broker and NTP host names */
#define GW_CRED_SIZE   65   /* client_id, username, password */
#define GW_UNITS_SIZE  16
#define GW_CFG_ERR_SIZE 192

typedef enum {
    GW_AREA_COIL = 0,
    GW_AREA_DISCRETE,
    GW_AREA_HOLDING,
    GW_AREA_INPUT,
} gw_area_t;

typedef enum {
    GW_TYPE_BOOL = 0,
    GW_TYPE_UINT16,
    GW_TYPE_INT16,
    GW_TYPE_UINT32,
    GW_TYPE_INT32,
    GW_TYPE_FLOAT32,
} gw_type_t;

typedef enum {
    GW_WORD_BIG = 0,    /* ABCD: high word in the lower register address */
    GW_WORD_LITTLE,     /* CDAB: low word in the lower register address */
} gw_word_order_t;

typedef enum {
    GW_PARITY_NONE = 0,
    GW_PARITY_EVEN,
    GW_PARITY_ODD,
} gw_parity_t;

typedef struct {
    char name[GW_NAME_SIZE];
    gw_area_t area;
    gw_type_t type;
    gw_word_order_t word_order;
    uint16_t address;
    bool writable;
    double scale;
    double offset;
    double deadband;
    char units[GW_UNITS_SIZE];  /* validated and kept; not published by the MVP (SPB-16) */
} gw_tag_cfg_t;

typedef struct {
    char name[GW_NAME_SIZE];    /* also the Sparkplug device_id */
    uint8_t unit_id;
    uint32_t poll_ms;
    uint16_t offline_after;
    uint16_t first_tag;         /* index into gw_config_t.tags */
    uint16_t tag_count;
} gw_device_cfg_t;

typedef struct {
    struct {
        bool dhcp;
        uint8_t ip[4];
        uint8_t netmask[4];
        uint8_t gateway[4];
        uint8_t dns[4];          /* 0.0.0.0 = not set */
    } network;
    struct {
        char server[GW_HOST_SIZE];
    } ntp;
    struct {
        char host[GW_HOST_SIZE];
        uint16_t port;
        char client_id[GW_CRED_SIZE];   /* already defaulted to edge_node_id when empty */
        char username[GW_CRED_SIZE];
        char password[GW_CRED_SIZE];
        uint16_t keepalive_s;
    } mqtt;
    struct {
        char group_id[GW_ID_SIZE];
        char edge_node_id[GW_ID_SIZE];
        char primary_host_id[GW_ID_SIZE];  /* parsed only; SPB-13 is not part of the MVP */
    } sparkplug;
    struct {
        uint32_t baud;
        gw_parity_t parity;
        uint8_t stop_bits;
        uint16_t response_timeout_ms;
        uint8_t retries;
        uint16_t inter_frame_delay_ms;
    } rs485;
    struct {
        uint16_t di_debounce_ms;
    } local_io;

    uint16_t device_count;
    uint16_t tag_count;
    gw_device_cfg_t devices[GW_MAX_DEVICES];
    gw_tag_cfg_t tags[GW_MAX_TAGS];
} gw_config_t;

typedef struct {
    char msg[GW_CFG_ERR_SIZE];  /* "<json path>: <reason>" */
} gw_cfg_err_t;

/**
 * Parse and validate a configuration document.
 *
 * Unknown keys are rejected (catches typos). Optional keys take the defaults of
 * the SPEC example. On failure @p err holds a descriptive message with the JSON
 * path of the offending value and @p cfg is left zeroed.
 */
bool gw_config_parse(const char *json, size_t len, gw_config_t *cfg, gw_cfg_err_t *err);

/**
 * Allocator for the temporary document tree built while parsing (default:
 * malloc/free). A spec-maximum document (256 tags) needs about 200 KB of tree,
 * so the firmware points this at PSRAM. Running out is reported as such, not as
 * a syntax error.
 */
void gw_config_set_allocator(void *(*alloc)(size_t size), void (*release)(void *ptr));

/** Index of the device named @p name (length @p len), or -1. */
int gw_config_find_device(const gw_config_t *cfg, const char *name, size_t len);

/** Global tag index of the tag named @p name inside device @p dev, or -1. */
int gw_config_find_tag(const gw_config_t *cfg, uint16_t dev, const char *name);

/** Device that owns global tag index @p tag. */
uint16_t gw_config_tag_device(const gw_config_t *cfg, uint16_t tag);

/** Number of Modbus registers (or bits, for coil/discrete) a tag occupies. */
uint16_t gw_tag_width(const gw_tag_cfg_t *tag);

#ifdef __cplusplus
}
#endif
