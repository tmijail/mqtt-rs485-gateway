/*
 * #205 #221: the rules of the certificate files, without mbedTLS.
 *
 * The PEM files of the LittleFS partition (next to config.json) are for the keys
 * mqtt.tls.ca_file, mqtt.tls.cert_file, mqtt.tls.key_file and ota.ca_file. The card Certificates
 * of the web UI loads, replaces and removes them. This module has the parts of it that need no
 * parser of certificates (the IDF adapters gw_pemfile and gw_webcert parse them with mbedTLS):
 *
 *   - the limits of the files (number and size);
 *   - the file names that the saved configuration uses (the column "Used by", and the refusal of
 *     the remove of a file in use);
 *   - the plan of a load: what the new content must be when the saved configuration uses the
 *     file, and the pair check of the client certificate and its key;
 *   - the private key block of a text that holds a certificate and its key together;
 *   - the check that the SANs of the HTTPS certificate contain the hostname (#205).
 *
 * Pure C on cJSON (gw_port_json_lock); no allocation of its own.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gw_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* #221, decision of the product owner: the implementation measures the free space of LittleFS
 * and proposes the limits (README 3.x, "The PEM files"). The product owner approves them later. */
#define GW_PEM_FILES_MAX      16      /* PEM files in the LittleFS partition */
#define GW_PEM_FILE_MAX_BYTES 16384   /* one PEM file (a chain of certificates, a key) */

typedef enum {
    GW_PEM_OTHER = 0,     /* not a PEM certificate or an unencrypted PEM private key */
    GW_PEM_CERTS,         /* one or more certificates */
    GW_PEM_KEY,           /* an unencrypted private key */
} gw_pem_kind_t;

/* The keys of the configuration document that name a PEM file. */
typedef enum {
    GW_FILEREF_MQTT_CA = 0,   /* mqtt.tls.ca_file */
    GW_FILEREF_MQTT_CERT,     /* mqtt.tls.cert_file */
    GW_FILEREF_MQTT_KEY,      /* mqtt.tls.key_file */
    GW_FILEREF_OTA_CA,        /* ota.ca_file */
    GW_FILEREF_COUNT
} gw_fileref_t;

typedef struct {
    char name[GW_FILEREF_COUNT][GW_FILE_SIZE];   /* "" = the key is absent or empty */
} gw_filerefs_t;

/** The JSON path of @p r, for example "mqtt.tls.ca_file". */
const char *gw_fileref_path(gw_fileref_t r);

/** The content that the key @p r needs: GW_PEM_KEY for mqtt.tls.key_file, else GW_PEM_CERTS. */
gw_pem_kind_t gw_fileref_kind(gw_fileref_t r);

/**
 * The file names of the four keys in the configuration document @p json (@p len bytes). Only the
 * syntax must be correct: a document that the firmware rejects (recovery mode) still names its
 * files. A value that is not a string counts as absent. False if the text is not a JSON object.
 */
bool gw_filerefs_parse(const char *json, size_t len, gw_filerefs_t *out);

/** The file names of the four keys of a parsed configuration. */
void gw_filerefs_from_config(const gw_config_t *cfg, gw_filerefs_t *out);

/** The keys that use the file @p name: bit (1u << gw_fileref_t) for each. 0 = not used. */
unsigned gw_filerefs_users(const gw_filerefs_t *refs, const char *name);

/**
 * The private key blocks ("-----BEGIN ... PRIVATE KEY-----" to its END line) of @p text. Returns
 * the number of blocks; for the first one, @p off and @p klen give its start and its length (to
 * the end of its END line). The rest of the text (before and after it) holds the certificates.
 */
unsigned gw_pem_key_blocks(const char *text, size_t len, size_t *off, size_t *klen);

/* One file of a load: its name and what the gateway found in it. */
typedef struct {
    const char *name;
    gw_pem_kind_t kind;
} gw_pemload_file_t;

/* What a load must still examine with mbedTLS (gw_pemload_plan). */
typedef struct {
    bool pair;           /* examine that the key agrees with the certificate: */
    const char *cert;    /*   the certificate file of the pair */
    const char *key;     /*   and its key file */
    bool cert_new;       /*   true: the certificate of the pair comes in the load, else stored */
    bool key_new;        /*   true: the key comes in the load, else the stored file */
    bool configured;     /*   the pair of the saved configuration (else the pair of the load) */
} gw_pemload_plan_t;

/**
 * The plan of a load of @p n files (1, or 2: a certificate in @p files[0] and its key in
 * @p files[1]) against the saved configuration @p refs. Refused (false, with the JSON path in
 * @p path, or "" for the request, and the reason in @p why):
 *   - two files with the same name, or a pair that is not a certificate and a key;
 *   - a file that a key of the saved configuration uses, with a content of the other kind (a key
 *     in place of a CA file, for example).
 * Else true, and @p plan says if a pair check is necessary: the client certificate and the
 * client key of the saved configuration when the load replaces one of them (the other one stays
 * the stored file if the load does not replace it too), or else the two files of the load.
 */
bool gw_pemload_plan(const gw_filerefs_t *refs, const gw_pemload_file_t *files, size_t n,
                     gw_pemload_plan_t *plan, char *path, size_t path_cap, char *why,
                     size_t why_cap);

/**
 * #205: the SANs @p sans ("DNS:gw01.local,DNS:gw01,IP:192.168.1.50", the text of the web
 * certificate) contain the hostname @p host: a DNS name that is @p host, "<host>.local" or
 * "<host>.<domain>" (case-insensitive). False for an empty @p host or no DNS name.
 */
bool gw_san_has_host(const char *sans, const char *host);

/* An NVS page has 126 entries of 32 bytes. A string uses one entry for its header, then entries
 * of 32 bytes for its text with the NUL, all in one page. */
#define GW_NVS_ENTRY_BYTES  32
#define GW_NVS_PAGE_ENTRIES 126

/** The NVS entries of a string of @p len bytes (without its NUL). */
size_t gw_nvs_str_entries(size_t len);

/**
 * #205: the free NVS entries (available_entries of nvs_get_stats) that a certificate that waits
 * for the restart needs before the gateway stores it. Its three strings (@p cert_len, @p key_len,
 * @p src_len bytes without the NUL) are in NVS two times at the next boot: the copy into the
 * certificate in use is written before the gateway erases the strings that wait. One more page
 * (GW_NVS_PAGE_ENTRIES) is for the fragmentation (a string does not go across two pages) and for
 * the other NVS writes (bdSeq, the local I/O state). The NVS partition has 6 pages (24 KiB), and
 * NVS keeps one of them free: two certificates of 3999 bytes with keys of 3999 bytes do not fit.
 */
size_t gw_webcert_nvs_need(size_t cert_len, size_t key_len, size_t src_len);

#ifdef __cplusplus
}
#endif
