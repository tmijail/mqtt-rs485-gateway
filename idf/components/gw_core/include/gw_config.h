/*
 * Gateway configuration: the common JSON schema of SPEC section 6, parsed and
 * validated into a fixed-size structure (no pointers, no heap after parsing).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gw_cfgschema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Static limits (SPEC section 6, "MVP limits"). */
#define GW_MAX_DEVICES          16
#define GW_MAX_TAGS_PER_DEVICE  64
#define GW_MAX_TAGS             256

/* Buffer sizes, including the terminating NUL. */
#define GW_NAME_SIZE   40   /* device and tag names */
#define GW_ID_SIZE     33   /* Sparkplug group_id / edge_node_id / primary_host_id */
#define GW_PREFIX_SIZE 97   /* #172: sparkplug.device_prefix, location levels with ':' between them */
#define GW_TOPIC_SIZE  160  /* longest Sparkplug topic, with its NUL (gw_node.h GW_TOPIC_MAX) */
#define GW_MDNS_INSTANCE_SIZE 64  /* #172: one DNS label, 1..63 bytes (RFC 6763 section 4.1.1) */
#define GW_HOST_SIZE   128  /* broker and NTP host names */
#define GW_CRED_SIZE   65   /* client_id, username, password */
#define GW_UNITS_SIZE  16
#define GW_SSID_SIZE   33   /* Wi-Fi SSID: 1..32 bytes */
#define GW_PSK_SIZE    65   /* Wi-Fi passphrase (8..63) or raw PSK (64 hex digits) */
#define GW_HOSTNAME_SIZE 64 /* NET-6: one DNS label, 1..63 characters */
#define GW_USER_SIZE   33   /* web user name */
#define GW_PWHASH_SIZE 128  /* "pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>" (gw_auth.h) */
#define GW_FILE_SIZE   33   /* name of a file next to config.json (TLS certificates, MQTT-4/5) */
#define GW_LABEL_SIZE  64   /* #206: gateway.name and gateway.location, 0..63 bytes */
#define GW_COMMENT_MAX 255  /* #206: gateway.comment, bytes (not kept: only the document has it) */
#define GW_MAX_BACKUP_BROKERS 3  /* MQTT-6: besides mqtt.host */
#define GW_CFG_ERR_SIZE 192
#define GW_CFG_PATH_SIZE 96
#define GW_JSON_MAX_DEPTH 32  /* deeper documents are rejected before cJSON recurses into them */

/* MB-6 strings: up to 16 registers, two characters per register. */
#define GW_STR_MAX_REGS 16
#define GW_STR_MAX      (2 * GW_STR_MAX_REGS)
#define GW_TAG_MAX_REGS GW_STR_MAX_REGS     /* widest tag: a string of 16 registers */
#define GW_NO_BIT       (-1)
#define GW_CFG_BUILD_DEFAULT (-1)   /* optional key absent: the build-time default applies */
#define GW_MAX_GAP_LIMIT 1999       /* a block never spans more than 2000 bits */
#define GW_MBTCP_MAX_ALLOWED 8      /* MB-15: entries of modbus_tcp.allowed_clients */
#define GW_LOCAL_IO_CHANNELS 8      /* DI/1..8 and DO/1..8 */
#define GW_FAILSAFE_MAX_S 86400     /* IO-3: local_io.failsafe_s */
#define GW_COUNTER_SAVE_MIN_S 60    /* IO-4: local_io.counter_save_s (or 0) */
#define GW_COUNTER_SAVE_MAX_S 86400
#define GW_DIAG_MIN_S 10             /* SPB-17: sparkplug.diagnostics_s (or 0) */
#define GW_DIAG_MAX_S 86400
#define GW_MAX_TEMPLATE_TAGS 64     /* SPB-18: tags of all template definitions together (NBIRTH) */

/* #169: sparkplug.naming. GW_NAMING_UMH is the UMH preset: the name checks of gw_config_lint()
 * become errors (and, with #173 and #174, more items that obey Sparkplug B 3.0.0). */
typedef enum {
    GW_NAMING_SPARKPLUG = 0,   /* default: no change for any host */
    GW_NAMING_UMH,
} gw_naming_t;

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
    GW_TYPE_BYTES,      /* SPB-19: `length` registers as raw bytes (Sparkplug Bytes) */
} gw_type_t;

/* Register and byte order of multi-byte values (MB-5, MB-6). Letters name the bytes of the value
 * from the most significant (A) down, in the order they arrive on the wire. */
typedef enum {
    GW_WORD_BIG = 0,    /* ABCD ("big"): most significant word first, bytes big-endian */
    GW_WORD_LITTLE,     /* CDAB ("little"): least significant word first */
    GW_WORD_BADC,       /* BADC: most significant word first, bytes swapped in each register */
    GW_WORD_DCBA,       /* DCBA: fully little-endian */
} gw_word_order_t;

/* IO-3: state of a DO at a restart and after local_io.failsafe_s without a Sparkplug session. */
typedef enum {
    GW_DO_SAFE_OFF = 0,   /* default */
    GW_DO_SAFE_ON,
    GW_DO_SAFE_HOLD,      /* keep the state it had (the expander's latch across a restart) */
} gw_do_safe_t;

/* IO-7 local rule DI -> DO. */
typedef enum {
    GW_RULE_FOLLOW = 0,   /* DO = DI */
    GW_RULE_INVERT,       /* DO = not DI */
    GW_RULE_TOGGLE,       /* every activation of the DI toggles the DO */
} gw_rule_mode_t;

/* IO-6 buzzer alarms; the value is also the number of beeps of its pattern and its priority. */
typedef enum {
    GW_ALARM_NONE = 0,
    GW_ALARM_DEVICE_OFFLINE = 1,   /* a Modbus device offline (MB-9) */
    GW_ALARM_MQTT_DOWN = 2,        /* no Sparkplug session */
    GW_ALARM_CONFIG_ERROR = 3,     /* configuration rejected (recovery) */
} gw_alarm_t;
#define GW_ALARM_BIT(a) (1u << (a))
#define GW_ALARM_AFTER_MAX_S 3600

typedef enum {
    GW_PARITY_NONE = 0,
    GW_PARITY_EVEN,
    GW_PARITY_ODD,
} gw_parity_t;

typedef enum {
    GW_MQTT_V311 = 0,     /* MQTT 3.1.1 (the MVP) */
    GW_MQTT_V5,           /* MQTT 5.0 (MQTT-8) */
} gw_mqtt_protocol_t;

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

/* A new field here or in gw_device_cfg_t: decide whether the store & forward hash covers it
 * (gw_sfcfg.c, #145; its guard fails the build until then). */
typedef struct {
    char name[GW_NAME_SIZE];
    gw_area_t area;
    gw_type_t type;
    gw_word_order_t word_order;
    uint16_t address;
    int8_t bit;                 /* MB-6: bit 0..15 of a holding/input register (bool), or GW_NO_BIT */
    uint8_t length;             /* MB-6: registers of a string or bytes tag (1..GW_STR_MAX_REGS),
                                 * else 0 */
    bool slow;                  /* MB-12: read every slow_poll_ms of its device ("scan": "slow") */
    bool writable;
    bool has_eng_low;           /* SPB-16: engLow / engHigh properties (optional); in the */
    bool has_eng_high;          /* padding before the doubles: no growth of the structure */
    bool has_write_min;         /* #196: write_min / write_max given (also in the padding) */
    bool has_write_max;
    double scale;
    double offset;
    double deadband;
    char units[GW_UNITS_SIZE];  /* SPB-16: property engUnit in the birth ("" = none) */
    float eng_low;              /* published as Float: 8 B per tag instead of 16 (256 tags) */
    float eng_high;
    /* #196: the range of a DCMD write, in engineering units (after scale and offset). Doubles,
     * because a limit of a 32-bit or 64-bit integer tag must be exact. */
    double write_min;
    double write_max;
} gw_tag_cfg_t;

typedef struct {
    char name[GW_NAME_SIZE];    /* also the Sparkplug device_id */
    char type[GW_NAME_SIZE];    /* SPB-18: template (device type) name; "" = no template */
    uint8_t unit_id;
    int8_t write_multiple;      /* #197: 1 = FC15/FC16 also for one coil or register, 0 = FC05/
                                 * FC06, or GW_CFG_BUILD_DEFAULT (CONFIG_GW_MB_FORCE_MULTI_WRITE);
                                 * in the padding after unit_id */
    bool enabled;               /* #214: false = the device stays in the configuration (and the
                                 * validation examines it), but is not polled and has no DBIRTH */
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
        gw_mqtt_protocol_t protocol;    /* MQTT-8 */
    } mqtt;
    struct {
        char group_id[GW_ID_SIZE];
        char edge_node_id[GW_ID_SIZE];
        char primary_host_id[GW_ID_SIZE];  /* SPB-13: "" = no primary host application */
        char device_prefix[GW_PREFIX_SIZE]; /* #172: "" or "<level>:<level>...": the device_id of
                                             * each device is "<device_prefix>:<name>" */
        gw_naming_t naming;                /* #169: GW_NAMING_UMH = the UMH preset */
        bool store_forward;                /* SPB-14: keep changes made while offline */
        bool sf_sd;                        /* SPB-14: spill the full ring to the microSD card */
        uint32_t sf_sd_max_kb;             /* bound of the spill on the card */
        uint32_t diagnostics_s;  /* SPB-17: NDATA with the node diagnostics this often (0 =
                                  * births only) */
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
        uint32_t failsafe_s;     /* IO-3: seconds without a Sparkplug session before the DOs
                                  * take their safe state; 0 = no reaction to an MQTT loss */
        uint8_t do_failsafe[GW_LOCAL_IO_CHANNELS];   /* gw_do_safe_t of DO/1..8 */
        uint32_t counter_save_s; /* IO-4: pulse counters saved to NVS this often (0 = never) */
        uint8_t rule_count;      /* IO-7: local DI -> DO rules, one per DO at most */
        struct {
            uint8_t di;          /* 0-based */
            uint8_t dout;        /* 0-based */
            uint8_t mode;        /* gw_rule_mode_t */
        } rules[GW_LOCAL_IO_CHANNELS];
        struct {                 /* IO-6 */
            uint8_t alarms;      /* GW_ALARM_BIT() of the enabled alarms; 0 = silent */
            uint16_t alarm_after_s;  /* MQTT down / device offline must last this long */
        } buzzer;
    } local_io;
    struct {                     /* REL-4; disabled when host is empty */
        char host[GW_HOST_SIZE];
        uint16_t port;
        gw_log_level_t level;
    } syslog;
    struct {                     /* CFG-3 / CFG-4: web UI and REST API */
        bool enabled;
        uint16_t port;                     /* HTTP: only redirects to HTTPS when https is on */
        bool https;                        /* issue #64: HTTPS with the device certificate */
        uint16_t https_port;
        char username[GW_USER_SIZE];
        char password[GW_CRED_SIZE];       /* clear text only while a new password is being
                                            * stored (CFG-7); empty at run time */
        char password_hash[GW_PWHASH_SIZE]; /* "" = no password set: commissioning password */
    } web;

    struct {                     /* FW-1: firmware updates (NCMD and web) */
        bool mqtt;               /* #223: the NCMD Node Control/Firmware Update is accepted (and
                                  * in the NBIRTH); default false. The web UI always can */
        char ca_file[GW_FILE_SIZE];  /* CA of the update server; "" = mqtt.tls.ca_file if
                                      * set, else the ESP-IDF bundle of public root CAs */
    } ota;

    struct {                     /* MB-15 Modbus TCP gateway; off unless enabled */
        bool enabled;
        uint16_t port;
        uint16_t idle_timeout_s; /* 0 = never close an idle client */
        uint8_t client_count;    /* 0 = any client may connect */
        struct {
            uint8_t ip[4];
            uint8_t prefix;      /* 0..32 */
        } clients[GW_MBTCP_MAX_ALLOWED];
    } modbus_tcp;

    struct {                     /* #206: for people; "" = none. gateway.comment is not kept */
        char name[GW_LABEL_SIZE];
        char location[GW_LABEL_SIZE];
    } gateway;

    uint16_t device_count;
    uint16_t tag_count;
    gw_device_cfg_t devices[GW_MAX_DEVICES];
    gw_tag_cfg_t tags[GW_MAX_TAGS];
} gw_config_t;

typedef struct {
    char msg[GW_CFG_ERR_SIZE];  /* "<json path>: <reason>" or "... line L, column C: ..." */
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
 * give the line and column ("JSON syntax error at line 3, column 15: ..."),
 * semantic errors the JSON path of the offending value
 * ("devices[0].tags[2].type: ...").
 */
bool gw_config_parse(const char *json, size_t len, gw_config_t *cfg, gw_cfg_err_t *err);

/* Issue #144 item 3: the highest configuration schema that this firmware reads. Raise it (and
 * add the new keys and enum values to the "since" table in gw_config.c) when a key or an enum
 * value is added. Images declare it in their capabilities block (gw_ota.h). */
#define GW_CONFIG_SCHEMA 2

/** gw_schema_init() with the "since" table of this firmware. */
void gw_config_schema_init(gw_schema_report_t *rep);

/** The "since" table of this firmware (@p n: its entries), for the host tests. */
const gw_since_t *gw_config_since_table(size_t *n);

/**
 * gw_config_parse() that also gives every key and accepted enum value of the document to
 * gw_schema_note() (@p rep, from gw_config_schema_init()). On success the report is valid;
 * on failure it stays not valid (the document was not examined to the end).
 */
bool gw_config_parse_schema(const char *json, size_t len, gw_config_t *cfg, gw_cfg_err_t *err,
                            gw_schema_report_t *rep);

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

/**
 * #172: the Sparkplug device_id of device @p dev: "<sparkplug.device_prefix>:<name>", or the
 * name without a prefix. False if it does not fit @p cap (the validation makes sure that it
 * fits GW_TOPIC_SIZE in a topic).
 */
bool gw_config_device_id(const gw_config_t *cfg, uint16_t dev, char *out, size_t cap);

/**
 * #172: the mDNS instance name "<group_id>/<edge_node_id>", cut at a UTF-8 character boundary
 * so that it has at most GW_MDNS_INSTANCE_SIZE - 1 bytes (one DNS label). @p cap must be at
 * least GW_MDNS_INSTANCE_SIZE.
 */
void gw_config_mdns_instance(const gw_config_t *cfg, char *out, size_t cap);

/** #169: severity of a finding of gw_config_lint(). */
typedef enum {
    GW_LINT_WARNING = 0,   /* the configuration is valid; a UNS host can change the name */
    GW_LINT_ERROR,         /* the UMH preset (sparkplug.naming "umh") refuses it */
} gw_lint_level_t;

/** One finding: @p path is the JSON path ("devices[0].name"), @p msg the reason. Return false
 *  to stop the calls. The strings are valid only during the call. */
typedef bool (*gw_lint_cb_t)(void *ctx, gw_lint_level_t level, const char *path,
                             const char *msg);

/**
 * #169: the name checks for UMH Core and other UNS hosts, on a parsed configuration. It calls
 * @p cb (NULL: none) once for each finding, and returns the count of all findings, also of
 * those after @p cb returned false. It runs at the validation, never in the publish path, and
 * allocates nothing. With the UMH preset, gw_config_parse() refuses the first GW_LINT_ERROR.
 */
size_t gw_config_lint(const gw_config_t *cfg, gw_lint_cb_t cb, void *ctx);

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

/** Issue #182: the value of a tag's "area", "type" or "word_order" name, as the configuration
 *  accepts it ("CDAB" -> GW_WORD_LITTLE). False if the name is not one of them. */
bool gw_config_area_of(const char *name, gw_area_t *out);
bool gw_config_type_of(const char *name, gw_type_t *out);
bool gw_config_word_order_of(const char *name, gw_word_order_t *out);
/** The configuration name of @p type ("float32"). */
const char *gw_config_type_name(gw_type_t type);

#ifdef __cplusplus
}
#endif
