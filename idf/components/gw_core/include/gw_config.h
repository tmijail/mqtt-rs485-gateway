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
#define GW_HOSTNAME_SIZE 64 /* NET-6: one DNS label, 1..63 characters */
#define GW_USER_SIZE   33   /* web user name */
#define GW_PWHASH_SIZE 128  /* "pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>" (gw_auth.h) */
#define GW_FILE_SIZE   33   /* name of a file next to config.json (TLS certificates, MQTT-4/5) */
#define GW_MAX_BACKUP_BROKERS 3  /* MQTT-6: besides mqtt.host */
#define GW_CFG_ERR_SIZE 192
#define GW_CFG_PATH_SIZE 96
#define GW_JSON_MAX_DEPTH 32  /* deeper documents are rejected before cJSON recurses into them */

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

/* MQTT-4 / MQTT-5: TLS towards the broker. Certificates and keys are PEM files in the
 * LittleFS partition, next to config.json (/cfg/<name>). */
typedef struct {
    bool enabled;
    char ca_file[GW_FILE_SIZE];       /* "" = the ESP-IDF bundle of public root CAs */
    char server_name[GW_HOST_SIZE];   /* name the broker certificate must carry; "" = its host */
    char cert_file[GW_FILE_SIZE];     /* MQTT-5: client certificate (mutual TLS); "" = none */
    char key_file[GW_FILE_SIZE];      /* its private key (PEM, unencrypted); with cert_file */
} gw_tls_cfg_t;

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
        char hostname[GW_HOSTNAME_SIZE];  /* NET-6: DHCP and mDNS host name; derived from
                                           * edge_node_id when the document leaves it empty */
        bool mdns;               /* NET-6: answer <hostname>.local and announce _http._tcp */
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
        gw_tls_cfg_t tls;               /* MQTT-4 / MQTT-5 */
        struct {                        /* MQTT-6: tried in order after mqtt.host */
            char host[GW_HOST_SIZE];
            uint16_t port;              /* defaults to mqtt.port */
        } backup[GW_MAX_BACKUP_BROKERS];
        uint8_t backup_count;
        uint8_t failover_after;         /* failed attempts on one broker before the next */
        uint32_t primary_return_s;      /* 0 = keep a working backup */
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
    struct {                     /* CFG-3 / CFG-4: web UI and REST API */
        bool enabled;
        uint16_t port;
        char username[GW_USER_SIZE];
        char password[GW_CRED_SIZE];       /* clear text only while a new password is being
                                            * stored (CFG-7); empty at run time */
        char password_hash[GW_PWHASH_SIZE]; /* "" = no password set: commissioning password */
    } web;

    uint16_t device_count;
    uint16_t tag_count;
    gw_device_cfg_t devices[GW_MAX_DEVICES];
    gw_tag_cfg_t tags[GW_MAX_TAGS];
} gw_config_t;

typedef struct {
    char msg[GW_CFG_ERR_SIZE];  /* "<json path>: <reason>" or "... línea L, columna C: ..." */
    char path[GW_CFG_PATH_SIZE]; /* JSON path of a semantic error ("devices[0].tags[1].type",
                                  * "$" for the root); "" for syntax and other errors */
    unsigned line, column;      /* 1-based position of a syntax error; 0 otherwise */
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

/**
 * NET-6 default host name from @p src (edge_node_id): lower case, every character outside
 * [a-z0-9-] becomes '-', no leading, trailing or repeated '-', at most 63 characters;
 * "gateway" if nothing is left.
 */
void gw_hostname_from(const char *src, char *out, size_t cap);

/** Deepest nesting of objects and arrays in @p json (strings are skipped). */
unsigned gw_json_depth(const char *json, size_t len);

/** Parse an IPv6 literal (RFC 4291 text form, "::" and a trailing dotted quad allowed, no
 * zone) into network byte order. */
bool gw_parse_ipv6(const char *s, uint8_t out[16]);

/**
 * True if @p name can name a file next to config.json: 1 to GW_FILE_SIZE - 1 characters out
 * of [A-Za-z0-9._-], not starting with '.', and not one of the configuration's own files.
 */
bool gw_config_file_name_ok(const char *name);

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
