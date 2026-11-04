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
#define GW_SSID_SIZE   33   /* Wi-Fi SSID: 1..32 bytes */
#define GW_PSK_SIZE    65   /* Wi-Fi passphrase (8..63) or raw PSK (64 hex digits) */
#define GW_CFG_ERR_SIZE 192

/* MB-6 strings: up to 16 registers, two characters per register. */
#define GW_STR_MAX_REGS 16
#define GW_STR_MAX      (2 * GW_STR_MAX_REGS)
#define GW_TAG_MAX_REGS GW_STR_MAX_REGS     /* widest tag: a string of 16 registers */
#define GW_NO_BIT       (-1)
#define GW_CFG_BUILD_DEFAULT (-1)   /* optional key absent: the build-time default applies */
#define GW_MAX_GAP_LIMIT 1999       /* a block never spans more than 2000 bits */

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
    GW_TYPE_UINT64,     /* MB-6: 4 registers */
    GW_TYPE_INT64,
    GW_TYPE_FLOAT64,
    GW_TYPE_STRING,     /* MB-6: `length` registers, two ASCII characters each */
} gw_type_t;

/* Register and byte order of multi-byte values (MB-5, MB-6). Letters name the bytes of the value
 * from the most significant (A) down, in the order they arrive on the wire. */
typedef enum {
    GW_WORD_BIG = 0,    /* ABCD ("big"): most significant word first, bytes big-endian */
    GW_WORD_LITTLE,     /* CDAB ("little"): least significant word first */
    GW_WORD_BADC,       /* BADC: most significant word first, bytes swapped in each register */
    GW_WORD_DCBA,       /* DCBA: fully little-endian */
} gw_word_order_t;

typedef enum {
    GW_PARITY_NONE = 0,
    GW_PARITY_EVEN,
    GW_PARITY_ODD,
} gw_parity_t;

typedef enum {
    GW_WIFI_BACKUP = 0,   /* Ethernet preferred; Wi-Fi carries the session while Ethernet is down */
    GW_WIFI_PRIMARY,      /* Wi-Fi preferred; Ethernet is the backup */
} gw_wifi_role_t;

/* Minimum severity forwarded to the remote syslog server (RFC 5424 severities). */
typedef enum {
    GW_LOG_ERROR = 3,
    GW_LOG_WARNING = 4,
    GW_LOG_INFO = 6,
    GW_LOG_DEBUG = 7,
} gw_log_level_t;

/* IPv4 settings of one interface (NET-2). */
typedef struct {
    bool dhcp;
    uint8_t ip[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4];          /* 0.0.0.0 = not set */
} gw_ipv4_cfg_t;

/* NET-7, applied to every interface. */
typedef struct {
    bool enabled;
    bool slaac;              /* stateless autoconfiguration from router advertisements */
    bool has_address;
    uint8_t address[16];     /* static address (Ethernet only), network byte order, /64 on-link */
    bool has_dns;
    uint8_t dns[16];
} gw_ipv6_cfg_t;

/* NET-4: Wi-Fi station as an alternative or backup of Ethernet. */
typedef struct {
    bool enabled;
    char ssid[GW_SSID_SIZE];
    char password[GW_PSK_SIZE];
    gw_wifi_role_t role;
    uint16_t failback_s;     /* the preferred interface must stay up this long to win back */
    gw_ipv4_cfg_t ipv4;
} gw_wifi_cfg_t;

typedef struct {
    char name[GW_NAME_SIZE];
    gw_area_t area;
    gw_type_t type;
    gw_word_order_t word_order;
    uint16_t address;
    int8_t bit;                 /* MB-6: bit 0..15 of a holding/input register (bool), or GW_NO_BIT */
    uint8_t length;             /* MB-6: registers of a string tag (1..GW_STR_MAX_REGS), else 0 */
    bool slow;                  /* MB-12: read every slow_poll_ms of its device ("scan": "slow") */
    bool writable;
    double scale;
    double offset;
    double deadband;
    char units[GW_UNITS_SIZE];  /* validated and kept; not published by the MVP (SPB-16) */
} gw_tag_cfg_t;

typedef struct {
    char name[GW_NAME_SIZE];    /* also the Sparkplug device_id */
    uint8_t unit_id;
    uint32_t poll_ms;           /* fast scan period (MB-10) */
    uint32_t slow_poll_ms;      /* MB-12: period of the tags with "scan": "slow" */
    uint16_t offline_after;
    /* MB-11 block reads of this device; GW_CFG_BUILD_DEFAULT = the build's setting (Kconfig) */
    int8_t block_reads;         /* 0 = one request per tag, 1 = merge, or build default */
    int16_t max_gap;            /* largest unused register/bit hole bridged, or build default */
    uint16_t first_tag;         /* index into gw_config_t.tags */
    uint16_t tag_count;
} gw_device_cfg_t;

typedef struct {
    struct {
        bool dhcp;               /* Ethernet IPv4 (the SPEC example's keys) */
        uint8_t ip[4];
        uint8_t netmask[4];
        uint8_t gateway[4];
        uint8_t dns[4];          /* 0.0.0.0 = not set */
        gw_ipv6_cfg_t ipv6;
        gw_wifi_cfg_t wifi;
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
    struct {                     /* REL-4; disabled when host is empty */
        char host[GW_HOST_SIZE];
        uint16_t port;
        gw_log_level_t level;
    } syslog;

    uint16_t device_count;
    uint16_t tag_count;
    gw_device_cfg_t devices[GW_MAX_DEVICES];
    gw_tag_cfg_t tags[GW_MAX_TAGS];
} gw_config_t;

typedef struct {
    char msg[GW_CFG_ERR_SIZE];  /* "<json path>: <reason>" or "... línea L, columna C: ..." */
} gw_cfg_err_t;

/**
 * Parse and validate a configuration document.
 *
 * Unknown and duplicated keys are rejected (catches typos and copy-paste
 * mistakes). Optional keys take the defaults of the SPEC example. On failure
 * @p err holds a descriptive message and @p cfg is left zeroed: syntax errors
 * give the line and column ("JSON mal formado en la línea 3, columna 15: ..."),
 * semantic errors the JSON path of the offending value
 * ("devices[0].tags[2].type: ...").
 */
bool gw_config_parse(const char *json, size_t len, gw_config_t *cfg, gw_cfg_err_t *err);

/**
 * Allocator for the temporary document tree built while parsing (default:
 * malloc/free). A spec-maximum document (256 tags) needs about 200 KB of tree,
 * so the firmware points this at PSRAM. Running out is reported as such, not as
 * a syntax error.
 */
void gw_config_set_allocator(void *(*alloc)(size_t size), void (*release)(void *ptr));

/** Parse an IPv6 literal (RFC 4291 text form, "::" and a trailing dotted quad allowed, no
 * zone) into network byte order. */
bool gw_parse_ipv6(const char *s, uint8_t out[16]);

/** Index of the device named @p name (length @p len), or -1. */
int gw_config_find_device(const gw_config_t *cfg, const char *name, size_t len);

/** Global tag index of the tag named @p name inside device @p dev, or -1. */
int gw_config_find_tag(const gw_config_t *cfg, uint16_t dev, const char *name);

/** Returned by gw_config_tag_device() for a tag index that belongs to no device. */
#define GW_NO_DEVICE UINT16_MAX

/** Device that owns global tag index @p tag, or GW_NO_DEVICE (callers must check). */
uint16_t gw_config_tag_device(const gw_config_t *cfg, uint16_t tag);

/** Number of Modbus registers (or bits, for coil/discrete) a tag occupies. */
uint16_t gw_tag_width(const gw_tag_cfg_t *tag);

#ifdef __cplusplus
}
#endif
