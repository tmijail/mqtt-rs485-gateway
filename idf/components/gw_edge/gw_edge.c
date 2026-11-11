#include "gw_edge.h"

#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw_backoff.h"
#include "gw_brokersel.h"
#include "gw_bus.h"
#include "gw_fw.h"
#include "gw_io.h"
#include "gw_net.h"
#include "gw_node.h"
#include "gw_phost.h"
#include "gw_status.h"
#include "gw_sfcard.h"
#include "gw_sfspill.h"
#include "gw_storage.h"
#include "esp_transport.h"
#include "lwip/sockets.h"
#include "mbedtls/asn1.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "mqtt_client.h"
#include "mqtt5_client.h"   /* after mqtt_client.h: it needs its types */
#include "psa/crypto.h"

static const char *TAG = "edge";

#define STACK_BYTES      6144   /* about 2.5 KB free at the births (log_stacks), templates included */
#define MAX_IDLE_MS      1000
#define NET_TIMEOUT_MS   5000   /* esp-mqtt socket timeout (one socket write) */
#define REBOOT_DELAY_MS  100    /* let the log drain */
#define QUIESCE_MS       3000   /* longest wait for the bus task to finish its transaction */
#define STABLE_MAX_MS    60000  /* a session is stable after one keepalive period, at most this */
#define SUBACK_TIMEOUT_MS 5000  /* NBIRTH waits this long at most for the SUBACK */
#define TLS_ERROR_LOG_MS 10000  /* a TLS setup that cannot work is logged this often */
#define PHOST_WAIT_MS    30000  /* with backup brokers: no STATE online this long -> next broker */
#define SF_MSGS_PER_LOOP 4      /* historical messages per loop: live data is not held back */
#define SPILL_GO_WAIT_MS 10000  /* #122/#124: the card work starts then without a network */
/* #136: the longest time a planned restart spends to move the RAM ring to the card (plus at most
 * one card operation). The work is at most 3/4 of the 40 000-record PSRAM ring: about 180 blocks,
 * 2 extent allocations and 6 superblocks. At the 1-5 ms of a block write (#124) that is under 1 s;
 * 5 s also takes an average of 25 ms per operation, or one FAT step of 1 s (the #124 limit for a
 * card that needs a task of its own) with the blocks. The watchdog is fed after each operation, so
 * the 15 s task watchdog does not set the limit: the operator who waits for the restart and the
 * OTA restart do (the new image is already set to boot; 5 s more do not change its trial). */
#define FLUSH_LIMIT_MS   5000
/* #136: a restart requested by another task (web, shell, OTA, configuration) waits this long at
 * most for the Sparkplug task to start it; then the esp_timer task restarts without NDEATH and
 * without the flush. Longer than one publish with its budget and socket timeout (8 + 5 s), the
 * longest normal pass of the loop; a hung task meets the 15 s task watchdog first. */
#define RESTART_ANSWER_MS 20000
#define STR_(x) #x
#define STR(x)  STR_(x)

enum { EV_WAKE = 0, EV_CMD, EV_WRITE_DONE };

typedef struct {
    uint8_t type;
    uint8_t ok;
    uint16_t arg;   /* slot (EV_CMD) or tag (EV_WRITE_DONE) */
} edge_ev_t;

typedef struct {
    uint16_t topic_len;
    uint16_t len;
    char topic[GW_TOPIC_MAX];
    uint8_t data[CONFIG_GW_CMD_MAX_PAYLOAD];
} cmd_slot_t;

/* ---- written by other tasks ---- */
static atomic_bool s_ip_up;
static atomic_bool s_time_synced;
static atomic_bool s_time_valid;    /* TIME-2: the clock came from the RTC at boot */
static atomic_bool s_mqtt_connected;
static atomic_bool s_io_dirty;
static atomic_uint s_conn_gen;
static atomic_uint s_disc_gen;
static atomic_uint s_dev_dirty;
static atomic_int s_suback;     /* msg_id of the last SUBACK, negated if a topic was refused */
static atomic_uint s_route_gen;
static atomic_bool s_restart_req;
static atomic_bool s_restart_discard;   /* #136: a factory reset: the RAM ring is not kept */
static esp_timer_handle_t s_restart_backstop;   /* #136: RESTART_ANSWER_MS */
static atomic_bool s_rebirth_req;
static atomic_bool s_fw_dirty;     /* FW-1: the update's state changed */

/* ---- publish time bound (issue #59): shared with the esp_timer task ---- */
static esp_timer_handle_t s_pub_timer;
static SemaphoreHandle_t s_pub_lock;
static StaticSemaphore_t s_pub_lock_buf;
/* ---- issue #73: MQTT 5 Maximum Packet Size of the broker (CONNACK), 0 = no limit ---- */
static atomic_uint s_max_packet;
static uint32_t s_oversize;          /* messages not published because of it */
static char s_oversize_topic[GW_TOPIC_MAX];
static uint32_t s_oversize_bytes;
static int64_t s_oversize_logged_us;
#define PUBLISH_OVERSIZE GW_NODE_PUBLISH_TOO_LARGE   /* not a connection failure (issue #76) */
static int s_pub_fd = -1;            /* socket of the publish in progress, -1 = none */
static uint16_t s_pub_port;          /* its local port: tells a reused descriptor apart */
static const char *s_pub_topic;
static size_t s_pub_len;
static atomic_bool s_pub_cut;

/* ---- queues and buffers (static, REL-2 / MQTT-7) ---- */
static QueueHandle_t s_q;
static StaticQueue_t s_q_buf;
static uint8_t s_q_storage[CONFIG_GW_EDGE_QUEUE_LEN * sizeof(edge_ev_t)];
static QueueHandle_t s_free_slots;
static StaticQueue_t s_free_buf;
static uint8_t s_free_storage[CONFIG_GW_CMD_SLOTS];
static cmd_slot_t s_slots[CONFIG_GW_CMD_SLOTS];
static StaticTask_t s_task_buf;
static StackType_t s_stack[STACK_BYTES / sizeof(StackType_t)];

/* ---- owned by the Sparkplug task ---- */
static const gw_config_t *s_cfg;
static gw_node_t *s_node;   /* ~21 KB, in PSRAM when present (gw_storage_big_alloc) */
static esp_mqtt_client_handle_t s_client;
static esp_mqtt_client_config_t s_mqtt_cfg;
static char s_death_topic[GW_TOPIC_MAX];
static char s_ncmd_topic[GW_TOPIC_MAX];
static char s_dcmd_topic[GW_TOPIC_MAX];
static char s_state_topic[GW_TOPIC_MAX];   /* SPB-13: "" without sparkplug.primary_host_id */
static uint8_t s_will[64];
static uint8_t s_bdseq;
static bool s_started;
static bool s_prev_ip;
static int64_t s_ip_since_us;
static uint32_t s_attempt;      /* failed attempts since the last stable session (backoff) */
static int64_t s_retry_at_us;   /* 0: no reconnect scheduled */
static int64_t s_session_up_us; /* when the current session's NBIRTH went out */
static int s_sub_msg_id;        /* > 0: SUBSCRIBE sent, NBIRTH waits for its SUBACK */
static int64_t s_sub_deadline_us;
static unsigned s_handled_conn;
static unsigned s_handled_disc;
static bool s_births_timed;     /* the current session's births carried synchronised time */
static int64_t s_fed_us;        /* last time this task fed the task watchdog */
static uint32_t s_max_unfed_ms; /* longest stretch without feeding since the last reset */
static uint32_t s_max_gap_ms;   /* the same since boot (#122: the shell, for the board tests) */
static unsigned s_handled_route;
static uint32_t s_diag_wait_ms = UINT32_MAX;   /* until the next diagnostics NDATA */
static struct ifreq s_ifr;      /* interface the client is bound to (NET-4, two interfaces only) */
static char *s_ca_pem;          /* MQTT-4: CA file, boot allocation (PSRAM if any), never freed */
static char *s_cert_pem;        /* MQTT-5: client certificate and key, the same way */
static char *s_key_pem;
/* Issue #70: what the X.509 verify callback (mqtt_task) saw in the current attempt; read when
 * the same task reports the failure. */
typedef struct {
    uint32_t flags;          /* OR of every certificate's verify flags */
    bool seen;               /* the broker presented a certificate */
    char subject[96];        /* of the broker certificate (depth 0) */
    char sans[96];           /* its DNS and IP names */
    mbedtls_x509_time valid_from, valid_to;
} tls_verify_t;
static tls_verify_t s_vf;
static mbedtls_x509_crt s_ca_chain;   /* mqtt.tls.ca_file, parsed once at boot */
static char s_setup_error[200]; /* why the settings cannot work (TLS files): no connection attempted */
static int64_t s_tls_logged_us;
static bool s_waiting_time_logged;
static gw_brokersel_t s_bsel;   /* MQTT-6: which broker of the list the next attempt goes to */
static gw_phost_t s_phost;      /* SPB-13: what the primary host last said */
static bool s_waiting_host;     /* subscribed; NBIRTH waits for the host's STATE online */
static int64_t s_host_wait_since_us;
static bool s_host_disconnect;  /* the session ends because of the host, not the network */
static gw_sf_t s_sf;            /* SPB-14: store & forward ring (storage allocated at boot) */
static bool s_sf_psram;
/* SPB-14: the ring's spill to the microSD card (optional; #124: allocated with its buffers, in
 * PSRAM, only when it starts: no internal .bss for it in a build without PSRAM) */
static gw_sfspill_t *s_spill;
static bool s_spill_on;
static uint32_t s_spill_kb;     /* the KB its layout uses */
static char s_spill_off[96];    /* why the spill is not on (sd enabled), for the shell */
static bool s_spill_go;         /* #124: the card work may run (the network is up, or a while
                                   after the boot): the recovery is not in app_main */
static bool s_spill_ready_logged, s_spill_fail_logged;
static uint32_t s_spill_gc_logged; /* gc_files at the last "deleted" log line */
static atomic_bool s_card_release; /* #124/#139: a format needs the card: stop using it */
static atomic_bool s_card_released;
static bool s_stable;           /* the current session lasted stable_ms(): it counts as working */
static char s_client_if[8];     /* interface the client was bound to ("" = one interface) */

/* ---- read by other tasks (gw_edge_get_status) ---- */
static portMUX_TYPE s_st_mux = portMUX_INITIALIZER_UNLOCKED;
static gw_edge_status_t s_st;

static void wake(void)
{
    const edge_ev_t ev = {.type = EV_WAKE};
    if (s_q != NULL) {        /* gw_fw may report before gw_edge_start() */
        xQueueSend(s_q, &ev, 0);  /* dropping is harmless: state lives in the flags */
    }
}

/* ---------- producer API ---------- */

void gw_edge_notify_ip(bool up)
{
    atomic_store(&s_ip_up, up);
    wake();
}

void gw_edge_notify_route(void)
{
    atomic_fetch_add(&s_route_gen, 1);
    wake();
}

/* TIME-2: a time worth timestamping with, from SNTP or from the RTC at boot. */
static bool time_ok(void)
{
    return atomic_load(&s_time_synced) || atomic_load(&s_time_valid);
}

bool gw_edge_time_synced(void)
{
    return atomic_load(&s_time_synced);
}

void gw_edge_notify_time_synced(void)
{
    if (!atomic_exchange(&s_time_synced, true)) {
        ESP_LOGI(TAG, "time synchronised");
    }
    wake();
}

void gw_edge_notify_device(uint16_t dev)
{
    unsigned bit = 1u << dev;
    if ((atomic_fetch_or(&s_dev_dirty, bit) & bit) == 0) {
        wake();
    }
}

void gw_edge_notify_io(void)
{
    if (!atomic_exchange(&s_io_dirty, true)) {
        wake();
    }
}

void gw_edge_notify_write_done(uint16_t tag, bool ok)
{
    const edge_ev_t ev = {.type = EV_WRITE_DONE, .ok = ok, .arg = tag};
    if (xQueueSend(s_q, &ev, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "write confirmation for tag %u dropped (queue full)", tag);
    }
}

/* #139: the format task needs the card (gw_sfcard): the Sparkplug task stops the spill at its
 * next pass, between two card operations, and gives the card up (spill_step). */
static void release_card(void)
{
    atomic_store(&s_card_release, true);
    wake();
}

/* #136: the Sparkplug task did not start the requested restart in time (it is busy or stuck):
 * restart from here (esp_timer task), with no flush of the RAM ring and no NDEATH. */
static void on_restart_backstop(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "restart requested %d s ago, but the Sparkplug task did not start it: restarting "
                  "now, without NDEATH; the samples in the RAM ring are lost",
             RESTART_ANSWER_MS / 1000);
    gw_bus_quiesce(1000);   /* no Modbus frame cut by the restart */
    esp_restart();
}

static void request_restart(bool keep_samples)
{
    if (!keep_samples) {
        atomic_store(&s_restart_discard, true);
    }
    if (!atomic_exchange(&s_restart_req, true) && s_restart_backstop != NULL) {
        esp_timer_start_once(s_restart_backstop, RESTART_ANSWER_MS * 1000ULL);
    }
    wake();
}

void gw_edge_request_restart(void)
{
    request_restart(true);
}

void gw_edge_request_restart_discard(void)
{
    request_restart(false);
}

void gw_edge_notify_firmware(void)
{
    if (!atomic_exchange(&s_fw_dirty, true)) {
        wake();
    }
}

bool gw_edge_request_rebirth(void)
{
    if (!atomic_load(&s_mqtt_connected)) {
        return false;
    }
    atomic_store(&s_rebirth_req, true);
    wake();
    return true;
}

/* ---------- esp-mqtt glue (runs in the MQTT task) ---------- */

static void enqueue_cmd(const esp_mqtt_event_t *ev)
{
    if (ev->current_data_offset != 0) {
        return;  /* continuation of a fragmented message, already rejected */
    }
    if (ev->data_len != ev->total_data_len || ev->data_len > CONFIG_GW_CMD_MAX_PAYLOAD ||
        ev->topic_len >= GW_TOPIC_MAX) {
        ESP_LOGW(TAG, "command of %d bytes dropped (limit %d)", ev->total_data_len,
                 CONFIG_GW_CMD_MAX_PAYLOAD);
        return;
    }
    uint8_t slot;
    if (xQueueReceive(s_free_slots, &slot, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command dropped: all %d buffers busy", CONFIG_GW_CMD_SLOTS);
        return;
    }
    cmd_slot_t *c = &s_slots[slot];
    memcpy(c->topic, ev->topic, (size_t)ev->topic_len);
    c->topic_len = (uint16_t)ev->topic_len;
    memcpy(c->data, ev->data, (size_t)ev->data_len);
    c->len = (uint16_t)ev->data_len;
    const edge_ev_t e = {.type = EV_CMD, .arg = slot};
    if (xQueueSend(s_q, &e, 0) != pdTRUE) {
        xQueueSend(s_free_slots, &slot, 0);
        ESP_LOGW(TAG, "command dropped: event queue full");
    }
}

static const char *broker_host(unsigned i);

/* Issue #77: the CA bundle checks the broker certificate in a verify callback that
 * esp_crt_bundle_attach() installs and that no public header declares. Instead of naming it,
 * tls_attach() lets the public esp_crt_bundle_attach() install it and keeps whatever callback
 * it installed (the mbedTLS configuration field, type-checked below), then chains it from
 * tls_verify(). A future IDF that renames the bundle's function changes nothing here; one that
 * changes the callback's signature or the field fails the build with the messages below.
 * Verified with ESP-IDF 6.1 (mbedTLS 4.1); see README 3.1 for the supported versions. */
typedef int (*tls_vrfy_fn)(void *, mbedtls_x509_crt *, int, uint32_t *);
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0) || ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(7, 0, 0)
#error "gw_edge: TLS verify chaining (issue #77) is verified with ESP-IDF 6.x only: check that " \
       "esp_crt_bundle_attach() still installs its check with mbedtls_ssl_conf_verify(), then " \
       "extend this range (README 3.1)"
#endif
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((mbedtls_ssl_config *)0)->MBEDTLS_PRIVATE(f_vrfy)), tls_vrfy_fn),
               "gw_edge (issue #77): mbedtls_ssl_config's verify callback changed type; the CA "
               "bundle chaining in tls_attach() must be revisited");
static tls_vrfy_fn s_bundle_vrfy;   /* the bundle's check, chained by tls_verify() */
static void *s_bundle_vrfy_ctx;

/* Issue #70: X.509 verify callback (mqtt_task, during the handshake). esp-tls reports the
 * verification flags only while the peer certificate is still around after a failure, which
 * TLS 1.3 does not keep: they are recorded here instead, with what the certificate says. */
static int tls_verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx;
    int r = 0;
    if (s_cfg->mqtt.tls.ca_file[0] == '\0' && s_bundle_vrfy != NULL) {
        r = s_bundle_vrfy(s_bundle_vrfy_ctx, crt, depth, flags);   /* bundle: it finds the CA */
    }
    s_vf.flags |= *flags;
    if (depth == 0) {
        s_vf.seen = true;
        mbedtls_x509_dn_gets(s_vf.subject, sizeof(s_vf.subject), &crt->subject);
        s_vf.valid_from = crt->valid_from;
        s_vf.valid_to = crt->valid_to;
        size_t n = 0;
        s_vf.sans[0] = '\0';
        for (const mbedtls_x509_sequence *c = &crt->subject_alt_names; c != NULL && c->buf.p;
             c = c->next) {
            uint8_t tag = (uint8_t)(c->buf.tag & ~(MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED));
            if (tag == MBEDTLS_X509_SAN_DNS_NAME) {
                n += (size_t)snprintf(s_vf.sans + n, n < sizeof(s_vf.sans) ? sizeof(s_vf.sans) - n : 0,
                                      "%sDNS:%.*s", n ? "," : "", (int)c->buf.len, c->buf.p);
            } else if (tag == MBEDTLS_X509_SAN_IP_ADDRESS && c->buf.len == 4) {
                n += (size_t)snprintf(s_vf.sans + n, n < sizeof(s_vf.sans) ? sizeof(s_vf.sans) - n : 0,
                                      "%sIP:%u.%u.%u.%u", n ? "," : "", c->buf.p[0], c->buf.p[1],
                                      c->buf.p[2], c->buf.p[3]);
            }
        }
    }
    return r;
}

/* crt_bundle_attach hook of esp-tls: the CA (file or bundle) and the verify callback above. */
static esp_err_t tls_attach(void *conf)
{
    if (s_cfg->mqtt.tls.ca_file[0] != '\0') {
        mbedtls_ssl_conf_ca_chain(conf, &s_ca_chain, NULL);
    } else {
        esp_err_t e = esp_crt_bundle_attach(conf);
        if (e != ESP_OK) {
            return e;
        }
        /* issue #77: keep the check the bundle installed and chain it */
        const mbedtls_ssl_config *c = conf;
        s_bundle_vrfy = c->MBEDTLS_PRIVATE(f_vrfy);
        s_bundle_vrfy_ctx = c->MBEDTLS_PRIVATE(p_vrfy);
        if (s_bundle_vrfy == NULL) {
            /* this IDF verifies the bundle some other way: replacing its callback would
             * accept any certificate, so the attempt fails instead */
            ESP_LOGE(TAG, "TLS: the CA bundle installed no verify callback (unsupported ESP-IDF, "
                          "issue #77): connection refused");
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    mbedtls_ssl_conf_verify(conf, tls_verify, NULL);
    return ESP_OK;
}

static void fmt_time(char *out, size_t cap, const mbedtls_x509_time *t)
{
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d UTC", t->year, t->mon, t->day, t->hour,
             t->min, t->sec);
}

/* MQTT-4 / issue #70: say why a TLS connection failed, in terms of the configuration, with the
 * certificate's names and dates, the expected name and the clock. */
static void log_tls_error(const esp_mqtt_error_codes_t *e)
{
    unsigned flags = (unsigned)e->esp_tls_cert_verify_flags | s_vf.flags;
    int stack = e->esp_tls_stack_err < 0 ? -e->esp_tls_stack_err : e->esp_tls_stack_err;
    const char *expected = s_cfg->mqtt.tls.server_name[0] ? s_cfg->mqtt.tls.server_name
                                                          : broker_host(s_bsel.current);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char clock[112], from[72], to[72];
    snprintf(clock, sizeof(clock), "clock %04d-%02d-%02d %02d:%02d:%02d UTC, SNTP %s",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
             atomic_load(&s_time_synced) ? "synchronised" : "NOT synchronised");
    fmt_time(from, sizeof(from), &s_vf.valid_from);
    fmt_time(to, sizeof(to), &s_vf.valid_to);
    const char *what = (flags & MBEDTLS_X509_BADCERT_EXPIRED)  ? "the broker certificate expired"
                       : (flags & MBEDTLS_X509_BADCERT_FUTURE) ? "the broker certificate is not valid yet"
                       : (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
                           ? "the broker certificate does not carry the expected name"
                       : (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
                           ? "the broker certificate is not signed by the configured CA"
                       : flags != 0 ? "the broker certificate was rejected"
                       : stack == -MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
                           ? "the broker certificate failed verification"
                           : "TLS handshake failed";
    ESP_LOGW(TAG, "TLS: %s (esp-tls %s, mbedTLS -0x%04x, verify flags 0x%x, errno %d)", what,
             esp_err_to_name(e->esp_tls_last_esp_err), (unsigned)stack, flags,
             e->esp_transport_sock_errno);
    if (s_vf.seen) {
        ESP_LOGW(TAG, "TLS: certificate %s [%s], valid %s to %s; expected name '%s' (%s); %s",
                 s_vf.subject, s_vf.sans, from, to, expected,
                 s_cfg->mqtt.tls.server_name[0] ? "mqtt.tls.server_name" : "the broker host",
                 clock);
    } else {
        ESP_LOGW(TAG, "TLS: no broker certificate checked; expected name '%s'; %s", expected,
                 clock);
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_t *ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
#if CONFIG_MQTT_PROTOCOL_5
        /* issue #73: what the broker accepts at most (0: no limit, or MQTT 3.1.1) */
        atomic_store(&s_max_packet, ev->protocol_ver == MQTT_PROTOCOL_V_5 && ev->property != NULL
                                        ? ev->property->server.maximum_packet_size
                                        : 0u);
#endif
        atomic_store(&s_mqtt_connected, true);
        atomic_fetch_add(&s_conn_gen, 1);
        wake();
        break;
    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&s_mqtt_connected, false);
        atomic_fetch_add(&s_disc_gen, 1);
        wake();
        break;
    case MQTT_EVENT_SUBSCRIBED:
        atomic_store(&s_suback, ev->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED
                                    ? -ev->msg_id
                                    : ev->msg_id);
        wake();
        break;
    case MQTT_EVENT_DATA:
        enqueue_cmd(ev);
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "broker refused the connection (code %d)",
                     ev->error_handle->connect_return_code);
        } else if (s_cfg->mqtt.tls.enabled &&
                   (ev->error_handle->esp_tls_last_esp_err != ESP_OK ||
                    ev->error_handle->esp_tls_cert_verify_flags != 0)) {
            log_tls_error(ev->error_handle);
        } else {
            ESP_LOGW(TAG, "MQTT transport error (errno %d)",
                     ev->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

/* ---------- task watchdog ---------- */

/* Feed the TWDT and keep the longest stretch without feeding (logged after the births). A
 * single loop can publish NBIRTH and up to 16 DBIRTHs (and then 16 DDATAs); on a slow but
 * live link each publish may take seconds, so the watchdog is fed after every publish, not
 * once per loop: only one publish has to fit in the watchdog period. */
static void feed_watchdog(void)
{
    int64_t now = esp_timer_get_time();
    if (s_fed_us != 0) {
        uint32_t gap = (uint32_t)((now - s_fed_us) / 1000);
        if (gap > s_max_unfed_ms) {
            s_max_unfed_ms = gap;
        }
        if (gap > s_max_gap_ms) {
            s_max_gap_ms = gap;
        }
    }
    s_fed_us = now;
    esp_task_wdt_reset();
}

/* ---------- publish time bound (issue #59) ---------- */

static uint16_t local_port(int fd)
{
    struct sockaddr_storage a;
    socklen_t n = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &n) != 0) {
        return 0;
    }
    return a.ss_family == AF_INET6 ? ntohs(((struct sockaddr_in6 *)&a)->sin6_port)
                                   : ntohs(((struct sockaddr_in *)&a)->sin_port);
}

/* esp_timer task: the publish has run for the whole budget. esp-mqtt goes on writing while
 * every write makes progress, so on a slow link one publish has no bound of its own: shut the
 * socket down, the blocked write fails and esp-mqtt closes the connection. The lock and the
 * local port make sure the descriptor is still that of the publish in progress. */
static void on_publish_budget(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_pub_lock, portMAX_DELAY);
    if (s_pub_fd >= 0 && local_port(s_pub_fd) == s_pub_port) {
        atomic_store(&s_pub_cut, true);
        ESP_LOGW(TAG, "publish of %u bytes to %s still running after %d ms: link too slow for a "
                      "Sparkplug session, closing the connection",
                 (unsigned)s_pub_len, s_pub_topic, CONFIG_GW_MQTT_PUBLISH_BUDGET_MS);
        shutdown(s_pub_fd, SHUT_RDWR);
    }
    xSemaphoreGive(s_pub_lock);
}

/* Every publish of the Sparkplug task goes through here: at most the budget plus the socket
 * timeout of the write in progress (5 s), below the 15 s task watchdog. */
/* Size of the MQTT PUBLISH packet (no MQTT 5 properties): fixed header, topic, packet id,
 * property length, payload. */
static uint32_t publish_packet_size(const char *topic, int len, int qos)
{
    uint32_t rem = 2u + (uint32_t)strlen(topic) + (qos > 0 ? 2u : 0u) +
                   (s_cfg->mqtt.protocol == GW_MQTT_V5 ? 1u : 0u) + (uint32_t)len;
    uint32_t hdr = 1u + (rem < 128u ? 1u : rem < 16384u ? 2u : rem < 2097152u ? 3u : 4u);
    return hdr + rem;
}

static int bounded_publish(const char *topic, const char *data, int len, int qos, bool retain)
{
    uint32_t max = atomic_load(&s_max_packet);
    uint32_t size = publish_packet_size(topic, len, qos);
    if (max != 0 && size > max) {
        /* issue #73: the broker would drop it (or the connection): never send it */
        s_oversize++;
        snprintf(s_oversize_topic, sizeof(s_oversize_topic), "%s", topic);
        s_oversize_bytes = size;
        int64_t now = esp_timer_get_time();
        if (s_oversize_logged_us == 0 || now - s_oversize_logged_us >= 60000000LL) {
            s_oversize_logged_us = now;
            ESP_LOGE(TAG, "%s: %lu bytes, more than the broker's Maximum Packet Size of %lu "
                          "(by %lu): not published (%lu so far)",
                     topic, (unsigned long)size, (unsigned long)max, (unsigned long)(size - max),
                     (unsigned long)s_oversize);
        }
        return PUBLISH_OVERSIZE;
    }
    esp_transport_handle_t t =
        esp_mqtt_client_get_transport(s_client, s_cfg->mqtt.tls.enabled ? "mqtts" : "mqtt");
    int fd = t != NULL ? esp_transport_get_socket(t) : -1;
    xSemaphoreTake(s_pub_lock, portMAX_DELAY);
    s_pub_fd = fd;
    s_pub_port = fd >= 0 ? local_port(fd) : 0;
    s_pub_topic = topic;
    s_pub_len = (size_t)len;
    xSemaphoreGive(s_pub_lock);
    int64_t t0 = esp_timer_get_time();
    if (fd >= 0) {
        esp_timer_start_once(s_pub_timer, CONFIG_GW_MQTT_PUBLISH_BUDGET_MS * 1000ULL);
    }
    int r = esp_mqtt_client_publish(s_client, topic, data, len, qos, retain);
    esp_timer_stop(s_pub_timer);
    xSemaphoreTake(s_pub_lock, portMAX_DELAY);
    s_pub_fd = -1;
    xSemaphoreGive(s_pub_lock);
    if (atomic_exchange(&s_pub_cut, false)) {
        ESP_LOGW(TAG, "publish of %d bytes to %s aborted: returned after %lu ms (budget %d ms)",
                 len, topic, (unsigned long)((esp_timer_get_time() - t0) / 1000),
                 CONFIG_GW_MQTT_PUBLISH_BUDGET_MS);
        r = -1;
    }
    feed_watchdog();
    return r;
}

/* ---------- node ops (run in the Sparkplug task) ---------- */

static bool s_in_births;        /* issue #71: start_births() is publishing */
static unsigned s_birth_failures;

static int op_publish(void *ctx, const char *topic, const uint8_t *data, size_t len, int qos,
                      bool retain)
{
    (void)ctx;
    int r = bounded_publish(topic, (const char *)data, (int)len, qos, retain);
    if (r == -1 && s_in_births) {   /* not PUBLISH_OVERSIZE: that one is not the connection */
        s_birth_failures++;
    }
    return r;
}

static bool op_set_do(void *ctx, uint8_t index, bool on)
{
    (void)ctx;
    return gw_io_set_do(index, on);
}

static bool op_request_write(void *ctx, uint16_t tag, const gw_tag_value_t *value)
{
    (void)ctx;
    return gw_bus_request_write(tag, value);
}

/* FW-1: NCMD "Node Control/Firmware Update"; the outcome arrives through gw_fw's status. */
static void op_firmware_update(void *ctx, const char *url)
{
    (void)ctx;
    char why[120];
    gw_fw_start_url(url, why, sizeof(why));
}

/* FW-1: the update's state and progress into the node metrics (NDATA when online). */
static void sync_firmware(void)
{
    if (atomic_exchange(&s_fw_dirty, false)) {
        /* this task only: off its 6 KB stack, and in RTC FAST memory, not in the DRAM heap's
         * way (issue #80) */
        static RTC_FAST_ATTR char text[GW_NODE_FW_STATE_MAX];
        uint8_t progress;
        gw_fw_get_state(text, sizeof(text), &progress);
        gw_node_set_firmware_status(s_node, text, progress);
    }
}

static void op_set_scan_rate(void *ctx, uint32_t ms)
{
    (void)ctx;
    gw_bus_set_scan_rate(ms);
}

static size_t op_write_room(void *ctx)
{
    (void)ctx;
    return gw_bus_write_room();
}

/* SPB-17 */
static void op_diagnostics(void *ctx, gw_node_diag_t *out)
{
    (void)ctx;
    out->uptime_s = (uint64_t)(esp_timer_get_time() / 1000000);
    out->free_heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->min_free_heap = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->has_rssi = gw_net_has_wifi();
    int8_t rssi;
    out->rssi_valid = out->has_rssi && gw_net_wifi_rssi(&rssi);
    out->rssi = out->rssi_valid ? rssi : 0;
    out->time_synced = atomic_load(&s_time_synced);   /* TIME-3: SNTP only, not the RTC */
}

/* #124: the replay position goes to the card (fewer duplicates), before a format. */
static void spill_checkpoint(void)
{
    if (s_spill_on && gw_sfspill_checkpoint(s_spill, (uint32_t)(esp_timer_get_time() / 1000))) {
        ESP_LOGI(TAG, "store & forward: replay position saved on the card");
    }
}

static uint32_t flush_clock(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_sleep(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t > 0 ? t : 1);
    feed_watchdog();
}

/*
 * #136: before a planned restart, the samples of the RAM ring go to the card (they are replayed
 * after the boot, before anything newer), then one superblock with the head and the replay
 * position. Bounded by FLUSH_LIMIT_MS; each card operation feeds the watchdog (s_spill->tick).
 * Runs once, in this task, which owns the card: no other task uses it. Not when a format of the
 * card was requested (the format task waits for the card, #139), after a factory reset (the next
 * boot runs another configuration: the data could not be replayed), after a saved configuration
 * with another configuration hash (#141: the next boot starts a new ring), without a spill, or
 * when it failed or has not finished its boot recovery: then the RAM ring is lost, as before. Not
 * inlined: it must not grow the frame of reconcile().
 */
static __attribute__((noinline)) void spill_before_restart(void)
{
    static bool done;
    if (done || !s_spill_on) {
        return;
    }
    done = true;
    unsigned ram = (unsigned)gw_sf_ram_samples(&s_sf);
    if (atomic_load(&s_card_release) || atomic_load(&s_card_released)) {
        ESP_LOGW(TAG, "store & forward: a format of the card was requested: the %u samples of the "
                      "RAM ring are not moved to the card (lost by the restart)", ram);
        return;
    }
    if (atomic_load(&s_restart_discard)) {
        ESP_LOGW(TAG, "store & forward: factory reset: the %u samples of the RAM ring are not moved "
                      "to the card", ram);
        return;
    }
    uint32_t saved;
    if (gw_storage_saved_cfg_hash(&saved) && saved != s_spill->cfg_hash) {
        /* #141: the next boot starts a new ring and would not replay what is moved now */
        ESP_LOGW(TAG, "store & forward: the saved configuration has another configuration hash "
                      "(%08lx, the ring has %08lx): the %u samples of the RAM ring are not moved to "
                      "the card (the next boot starts a new ring)",
                 (unsigned long)saved, (unsigned long)s_spill->cfg_hash, ram);
        return;
    }
    gw_sf_flush_t r;
    gw_sf_flush(&s_sf, flush_clock, flush_sleep, FLUSH_LIMIT_MS, &r);
    unsigned stack = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    switch (r.result) {
    case GW_SF_FLUSH_DONE:
        ESP_LOGW(TAG, "store & forward: before the restart, %lu samples (%lu records) moved from "
                      "the RAM ring to the card in %lu ms (%lu blocks, %lu card operations); "
                      "sparkplug stack %u B never used",
                 (unsigned long)r.moved, (unsigned long)r.records, (unsigned long)r.ms,
                 (unsigned long)r.blocks, (unsigned long)r.steps, stack);
        break;
    case GW_SF_FLUSH_TIMEOUT:
        ESP_LOGE(TAG, "store & forward: before the restart, the time limit of %d ms expired: %lu "
                      "samples moved to the card (%lu blocks), %lu samples stay in the RAM ring and "
                      "are lost",
                 FLUSH_LIMIT_MS, (unsigned long)r.moved, (unsigned long)r.blocks,
                 (unsigned long)r.left);
        break;
    case GW_SF_FLUSH_FAILED:
        ESP_LOGE(TAG, "store & forward: before the restart, the card failed (%s) after %lu ms: %lu "
                      "samples moved to the card, %lu samples of the RAM ring are lost",
                 s_spill->why, (unsigned long)r.ms, (unsigned long)r.moved, (unsigned long)r.left);
        break;
    case GW_SF_FLUSH_NOT_READY:
        ESP_LOGW(TAG, "store & forward: the card is not read yet (boot recovery): the %lu samples of "
                      "the RAM ring are not moved to it (lost by the restart)",
                 (unsigned long)r.left);
        break;
    default:
        ESP_LOGW(TAG, "store & forward: the spill failed before (%s): the %lu samples of the RAM "
                      "ring are lost by the restart",
                 s_spill->why, (unsigned long)r.left);
        break;
    }
}

static void op_reboot(void *ctx)
{
    (void)ctx;
    spill_before_restart();
    gw_fw_restart_requested();   /* issue #108 B5 (a no-op unless a confirmation keeps failing) */
    /* Orderly shutdown: NDEATH was just published; stop() sends DISCONNECT synchronously so
     * the broker discards the will instead of publishing a second NDEATH. */
    esp_mqtt_client_stop(s_client);
    /* No Modbus frame cut by the restart (bounded by the slowest transaction with retries). */
    gw_bus_quiesce(QUIESCE_MS);
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();
}

/* ---------- connection management ---------- */

/* MQTT-6: broker @p i of the list (0 = mqtt.host, then mqtt.backup_brokers in order). */
static const char *broker_host(unsigned i)
{
    return i == 0 ? s_cfg->mqtt.host : s_cfg->mqtt.backup[i - 1].host;
}

static uint16_t broker_port(unsigned i)
{
    return i == 0 ? s_cfg->mqtt.port : s_cfg->mqtt.backup[i - 1].port;
}

static void publish_status(void)
{
    portENTER_CRITICAL(&s_st_mux);
    s_st.broker = s_bsel.current;
    s_st.broker_count = s_bsel.count;
    s_st.host = broker_host(s_bsel.current);
    s_st.port = broker_port(s_bsel.current);
    s_st.failures = s_bsel.failures;
    s_st.host_state = !s_phost.have_ts ? -1 : s_phost.online ? 1 : 0;
    s_st.waiting_host = s_waiting_host;
    s_st.sf_enabled = s_node != NULL && s_node->sf != NULL;
    s_st.sf_psram = s_sf_psram;
    s_st.sf_count = (uint32_t)gw_sf_count(&s_sf);
    s_st.sf_cap = (uint32_t)s_sf.cap;
    s_st.sf_dropped = s_sf.dropped;
    s_st.sf_sent = s_node != NULL ? s_node->sf_sent : 0;
    s_st.spill_on = s_spill_on;
    s_st.spilled = s_sf.spilled;
    s_st.spill_kb = s_spill_kb;
    const gw_sfspill_t *sp = s_spill_on ? s_spill : NULL;
    const char *off = sp != NULL && !sp->ok ? sp->why : s_spill_off;
    size_t off_len = strnlen(off, sizeof(s_st.spill_off) - 1);   /* no printf with IRQs off */
    memcpy(s_st.spill_off, off, off_len);
    s_st.spill_off[off_len] = '\0';
    if (sp != NULL) {
        s_st.spill_ready = sp->phase == GW_SFSPILL_READY;
        s_st.spill_failed = !sp->ok;
        s_st.spill_released = atomic_load(&s_card_released);
        s_st.spill_count = (uint32_t)gw_sfspill_waiting(sp);
        s_st.spill_dropped = sp->dropped;
        s_st.spill_stranded = (uint32_t)sp->stranded;
        s_st.spill_holes = sp->holes;
        s_st.spill_blocks = sp->blocks;
        s_st.spill_nblocks = sp->nblocks;
        s_st.spill_extents = sp->extents;
        s_st.spill_ext_blocks = sp->ext_blocks;
        s_st.spill_waiting_blocks = (uint32_t)(sp->head - sp->tail);
        s_st.spill_sb_writes = sp->sb_writes;
        s_st.spill_checkpoints = sp->checkpoints;
        s_st.spill_ck_max = sp->ck_max;
        s_st.spill_boot_ms = sp->boot_ms;
        s_st.spill_boot_reads = sp->scan_reads;
        s_st.spill_found = gw_sfspill_found_text(sp);   /* a constant string */
        s_st.spill_gc_pending = gw_sfspill_gc_pending(sp);
        s_st.spill_gc_files = sp->gc_files;
        s_st.spill_gc_steps = sp->gc_steps;
        s_st.spill_gc_left = sp->gc_left;
    }
    s_st.max_unfed_ms = s_max_gap_ms;
    s_st.max_packet = atomic_load(&s_max_packet);
    s_st.oversize = s_oversize;
    s_st.oversize_bytes = s_oversize_bytes;
    snprintf(s_st.oversize_topic, sizeof(s_st.oversize_topic), "%s", s_oversize_topic);
    portEXIT_CRITICAL(&s_st_mux);
}

void gw_edge_get_status(gw_edge_status_t *out)
{
    portENTER_CRITICAL(&s_st_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_st_mux);
}

static void prepare_attempt(void)
{
    memset(&s_vf, 0, sizeof(s_vf));   /* issue #70: what the next handshake will see */
    s_bdseq = gw_storage_next_bdseq();
    size_t n = gw_node_death_payload(s_bdseq, s_will, sizeof(s_will));
    s_mqtt_cfg.session.last_will.msg = (const char *)s_will;
    s_mqtt_cfg.session.last_will.msg_len = (int)n;
    /* MQTT-6: set_config (reconnect) or init (start_client) takes the current broker */
    s_mqtt_cfg.broker.address.hostname = broker_host(s_bsel.current);
    s_mqtt_cfg.broker.address.port = broker_port(s_bsel.current);
    publish_status();
    const char *v5 = s_cfg->mqtt.protocol == GW_MQTT_V5 ? " MQTT 5" : "";
    if (s_bsel.count > 1) {
        ESP_LOGI(TAG, "connecting to %s:%u%s%s (broker %u/%u) as '%s', bdSeq=%u",
                 broker_host(s_bsel.current), broker_port(s_bsel.current),
                 s_cfg->mqtt.tls.enabled ? " (TLS)" : "", v5, s_bsel.current + 1, s_bsel.count,
                 s_cfg->mqtt.client_id, s_bdseq);
    } else {
        ESP_LOGI(TAG, "connecting to %s:%u%s%s as '%s', bdSeq=%u", s_cfg->mqtt.host,
                 s_cfg->mqtt.port, s_cfg->mqtt.tls.enabled ? " (TLS)" : "", v5,
                 s_cfg->mqtt.client_id, s_bdseq);
    }
}

static void schedule_retry(int64_t now);

static void start_client(int64_t now)
{
    prepare_attempt();
    s_retry_at_us = 0;
    /* NET-4: with two interfaces the socket is bound to the active one, so the session really
     * runs where gw_net says (both may reach the broker). The binding is copied by
     * esp_mqtt_client_init and only changes with a new client (move_session). */
    s_mqtt_cfg.network.if_name = gw_net_active_ifname(s_ifr.ifr_name, sizeof(s_ifr.ifr_name)) ? &s_ifr
                                                                                    : NULL;
    s_client_if[0] = '\0';
    if (s_mqtt_cfg.network.if_name != NULL) {
        ESP_LOGI(TAG, "MQTT bound to interface %s", s_ifr.ifr_name);
        snprintf(s_client_if, sizeof(s_client_if), "%.*s", (int)sizeof(s_client_if) - 1,
                 s_ifr.ifr_name);
    }
    s_client = esp_mqtt_client_init(&s_mqtt_cfg);
    s_mqtt_cfg.network.if_name = NULL;   /* esp_mqtt_set_config would allocate it again */
    if (s_client == NULL) {
        /* Out of memory: retry with the same backoff as a failed connection (each attempt
         * takes a bdSeq and writes NVS, so it must not run once per loop). */
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        schedule_retry(now);
        return;
    }
#if CONFIG_MQTT_PROTOCOL_5
    if (s_cfg->mqtt.protocol == GW_MQTT_V5) {
        /* MQTT-8, Sparkplug 3.0 over MQTT 5: Clean Start (esp-mqtt's clean session, on by
         * default) and Session Expiry Interval 0, so the broker keeps no session state between
         * connections, as with clean session in 3.1.1; Will Delay Interval 0, so the NDEATH will
         * is published as soon as the connection is lost. Problem information is requested so
         * a refusal comes with its reason string. */
        const esp_mqtt5_connection_property_config_t props = {
            .session_expiry_interval = 0,
            .will_delay_interval = 0,
            .request_problem_info = true,
        };
        ESP_ERROR_CHECK(esp_mqtt5_client_set_connect_property(s_client, &props));
    }
#endif
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
    s_started = true;
}

static void schedule_retry(int64_t now)
{
    uint32_t delay = gw_backoff_ms(++s_attempt, CONFIG_GW_MQTT_BACKOFF_MIN_MS,
                                   CONFIG_GW_MQTT_BACKOFF_MAX_MS, esp_random());
    s_retry_at_us = now + (int64_t)delay * 1000;
    ESP_LOGW(TAG, "MQTT down; reconnect attempt %lu in %lu ms", (unsigned long)s_attempt,
             (unsigned long)delay);
}

/* Issue #58: esp-mqtt 1.1.0 (mqtt_client.c:633) callocs a new network.if_name on every
 * esp_mqtt_set_config() without freeing the previous one (about 17 B per call, one call per
 * connection attempt). The interface binding is therefore given to esp_mqtt_client_init()
 * only, and every set_config goes through here: a configuration that still carries it is
 * refused (and aborts debug builds) instead of leaking. test/host/check_set_config.cmake fails
 * the host tests if esp_mqtt_set_config() is called anywhere else. Remove when an esp-mqtt
 * release frees it (reported upstream; README 6.5). The same function also copies
 * credentials.authentication.key_password every time without freeing it: never set. */
static esp_err_t set_config(void)
{
    if (s_mqtt_cfg.network.if_name != NULL ||
        s_mqtt_cfg.credentials.authentication.key_password != NULL) {
        ESP_LOGE(TAG, "esp_mqtt_set_config with if_name/key_password would leak: refused");
        assert(false);
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_set_config(s_client, &s_mqtt_cfg);
}

static void reconnect(int64_t now)
{
    s_retry_at_us = 0;
    prepare_attempt();
    if (set_config() != ESP_OK ||
        esp_mqtt_client_reconnect(s_client) != ESP_OK) {
        schedule_retry(now);
    }
}

/* Orderly end of a Sparkplug session: NDEATH with the session's bdSeq (the will's payload). */
static void publish_ndeath(void)
{
    bounded_publish(s_death_topic, (const char *)s_will, s_mqtt_cfg.session.last_will.msg_len, 1,
                    false);
}

/* Close a session that cannot work; the DISCONNECTED event schedules the retry (backoff).
 * DISCONNECT makes the broker drop the will: no NBIRTH went out, so no NDEATH is owed. */
static void abort_session(const char *why)
{
    ESP_LOGW(TAG, "%s: closing the connection", why);
    s_sub_msg_id = 0;
    esp_mqtt_client_disconnect(s_client);
}

/* CONNACK received: subscribe to NCMD and DCMD; the births wait for the SUBACK (on_subscribed),
 * so a node that is announced online always receives its commands. */
static void on_connected(void)
{
    /* SPB-13: the host's STATE too, in the same SUBSCRIBE: its retained message comes right
     * after the SUBACK */
    const esp_mqtt_topic_t topics[] = {
        {.filter = s_ncmd_topic, .qos = 1},
        {.filter = s_dcmd_topic, .qos = 1},
        {.filter = s_state_topic, .qos = 1},
    };
    atomic_store(&s_suback, 0);
    gw_phost_session_start(&s_phost);
    s_waiting_host = false;
    int id = esp_mqtt_client_subscribe_multiple(s_client, topics, s_state_topic[0] ? 3 : 2);
    feed_watchdog();
    if (id < 0) {
        abort_session(s_state_topic[0] ? "SUBSCRIBE to NCMD/DCMD/STATE could not be sent"
                                       : "SUBSCRIBE to NCMD/DCMD could not be sent");
        return;
    }
    s_sub_msg_id = id;
    s_sub_deadline_us = esp_timer_get_time() + SUBACK_TIMEOUT_MS * 1000LL;
}

/* Stack margins of the tasks the MQTT session runs on (TLS handshakes are deep in mqtt_task). */
static void log_stacks(void)
{
    TaskHandle_t mqtt = xTaskGetHandle("mqtt_task");
    ESP_LOGI(TAG, "stack free (high-water mark): sparkplug %u of %u B, mqtt_task %u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)STACK_BYTES,
             mqtt != NULL ? (unsigned)uxTaskGetStackHighWaterMark(mqtt) : 0u);
}

/* The Sparkplug session starts: NBIRTH and DBIRTHs. */
static void start_births(void)
{
    /* s_attempt is not reset here: a broker that accepts the connection and drops it at once
     * (ACL, duplicate client_id, connection limit) must still see the backoff grow. It is
     * reset once the session has stayed up for a while (reconcile). */
    /* Issue #71: the SUBACK can be handled just before the transport error behind it: births
     * on a connection that is gone are lost, and "session up" would be a lie. */
    if (!atomic_load(&s_mqtt_connected) || atomic_load(&s_disc_gen) != s_handled_disc) {
        ESP_LOGW(TAG, "SUBACK received but the connection is already gone: no births");
        return;
    }
    s_retry_at_us = 0;
    atomic_store(&s_dev_dirty, 0);   /* the births carry the current state */
    atomic_store(&s_io_dirty, false);
    s_births_timed = time_ok();
    feed_watchdog();
    s_max_unfed_ms = 0;
    int64_t t0 = esp_timer_get_time();
    s_in_births = true;
    s_birth_failures = 0;
    uint32_t oversize_before = s_oversize;
    bool born = gw_node_session_up(s_node, s_bdseq);
    s_in_births = false;
    if (!born && s_oversize != oversize_before) {
        /* the NBIRTH itself is too large for this broker: no session can work here */
        abort_session("NBIRTH larger than the broker's Maximum Packet Size");
        return;
    }
    if (!born) {
        ESP_LOGW(TAG, "NBIRTH not published (bdSeq=%u): the connection dropped, the session is "
                      "not up",
                 s_bdseq);
        return;   /* the DISCONNECTED event schedules the retry */
    }
    if (s_birth_failures > 0 || !atomic_load(&s_mqtt_connected) ||
        atomic_load(&s_disc_gen) != s_handled_disc) {
        /* the NBIRTH went out but the connection died under the DBIRTHs: hosts would see a
         * node without its devices; the new session repeats every birth */
        gw_node_session_down(s_node);
        ESP_LOGW(TAG, "births incomplete (bdSeq=%u, %u publish(es) failed): the connection "
                      "dropped, the session is not up",
                 s_bdseq, s_birth_failures);
        return;
    }
    /* born: the NBIRTH fitted, so whatever was too large since was a DBIRTH (issue #76: the
     * session stays without those devices) */
    unsigned not_born = (unsigned)(s_oversize - oversize_before);
    s_session_up_us = esp_timer_get_time();
    gw_status_set(GW_ST_MQTT_UP, true);
    ESP_LOGI(TAG, "Sparkplug session up (bdSeq=%u): births sent in %lu ms, longest stretch "
             "without feeding the watchdog %lu ms", s_bdseq,
             (unsigned long)((esp_timer_get_time() - t0) / 1000), (unsigned long)s_max_unfed_ms);
    log_stacks();
    gw_fw_session_up(not_born);   /* FW-1: a new image on trial is healthy if every birth went out */
    if (gw_sf_count(&s_sf) > 0) {
        ESP_LOGI(TAG, "store & forward: %u sample(s) stored while offline follow as historical "
                      "data (%lu overwritten)",
                 (unsigned)gw_sf_count(&s_sf), (unsigned long)s_sf.dropped);
    }
}

/* SUBACK received for every topic: the session starts, once the primary host (if one is
 * configured) has said it is online (SPB-13). */
static void on_subscribed(void)
{
    if (s_state_topic[0] != '\0' && !s_phost.online) {
        s_waiting_host = true;
        s_host_wait_since_us = esp_timer_get_time();
        ESP_LOGI(TAG, "waiting for the primary host '%s' (STATE online) before NBIRTH",
                 s_cfg->sparkplug.primary_host_id);
        publish_status();
        return;
    }
    start_births();
}

/* SPB-13: a message on spBv1.0/STATE/<primary_host_id>. */
static void on_host_state(const uint8_t *data, size_t len)
{
    if (!atomic_load(&s_mqtt_connected)) {
        return;
    }
    uint64_t last = s_phost.last_ts;
    const char *id = s_cfg->sparkplug.primary_host_id;
    switch (gw_phost_on_state(&s_phost, data, len)) {
    case GW_PHOST_INVALID:
        ESP_LOGW(TAG, "STATE of '%s' ignored: not a Sparkplug 3.0 payload "
                      "({\"online\":bool,\"timestamp\":ms})", id);
        break;
    case GW_PHOST_STALE:
        ESP_LOGW(TAG, "STATE of '%s' ignored: older than the last one (timestamp %llu)", id,
                 (unsigned long long)last);
        break;
    case GW_PHOST_ONLINE:
        ESP_LOGI(TAG, "primary host '%s' online (timestamp %llu)", id,
                 (unsigned long long)s_phost.last_ts);
        if (s_waiting_host) {
            s_waiting_host = false;
            start_births();
        }
        break;
    case GW_PHOST_OFFLINE:
        ESP_LOGW(TAG, "primary host '%s' offline (timestamp %llu)", id,
                 (unsigned long long)s_phost.last_ts);
        if (s_node->online) {
            /* Sparkplug 3.0: NDEATH and disconnect; the next session waits for it again */
            publish_ndeath();
            gw_node_session_down(s_node);
            gw_status_set(GW_ST_MQTT_UP, false);
            s_host_disconnect = true;
            abort_session("the primary host went offline: NDEATH published");
        } else if (s_waiting_host && s_bsel.count > 1) {
            s_host_disconnect = true;
            abort_session("the primary host is offline on this broker");
        }
        break;
    }
    publish_status();
}

/* How long a session must last before the reconnect backoff starts again from the minimum. */
static uint32_t stable_ms(void)
{
    uint32_t ms = (uint32_t)s_cfg->mqtt.keepalive_s * 1000u;
    return ms < STABLE_MAX_MS ? ms : STABLE_MAX_MS;
}

/* NET-4: the active interface changed. A TCP connection cannot migrate, so the session ends
 * here and a new one (new bdSeq, NBIRTH, DBIRTHs) starts over the new interface at once.
 * MQTT-6 uses the same orderly end to return to the primary broker. */
static void move_session(const char *why)
{
    ESP_LOGW(TAG, "%s: new MQTT session", why);
    if (s_node->online) {
        /* Orderly end, as for a reboot: hosts learn it now and not after a keepalive. Over a
         * dead interface this is lost and the broker publishes the will later; the bdSeq of
         * the new session tells the two apart. */
        publish_ndeath();
        gw_node_session_down(s_node);
        gw_status_set(GW_ST_MQTT_UP, false);
    }
    esp_mqtt_client_destroy(s_client);   /* DISCONNECT if connected; drops the old binding */
    s_client = NULL;
    s_started = false;
    atomic_store(&s_mqtt_connected, false);
    s_handled_conn = atomic_load(&s_conn_gen);
    s_handled_disc = atomic_load(&s_disc_gen);
    s_retry_at_us = 0;
    s_attempt = 0;   /* a new interface or broker starts a fresh backoff */
    s_sub_msg_id = 0;
    s_stable = false;
    s_waiting_host = false;
}

/* An attempt ended without a session (or a session dropped): MQTT-6 failover, and issue #57
 * when no broker at all answers over this interface. */
static void attempt_ended(void)
{
    s_waiting_host = false;
    if (s_host_disconnect) {
        /* SPB-13: the network works; with backups the host may be on another broker */
        s_host_disconnect = false;
        s_stable = false;
        gw_brokersel_session_down(&s_bsel);
        if (s_bsel.count > 1 && gw_brokersel_failed(&s_bsel)) {
            ESP_LOGW(TAG, "primary host not online on broker: next attempt on broker %u/%u (%s:%u)",
                     s_bsel.current + 1, s_bsel.count, broker_host(s_bsel.current),
                     broker_port(s_bsel.current));
            s_attempt = 0;
        }
        publish_status();
        return;
    }
    if (s_stable) {
        s_stable = false;
        gw_brokersel_session_down(&s_bsel);   /* a working broker: the next try goes there */
        return;
    }
    unsigned from = s_bsel.current;
    unsigned fails = s_bsel.failures + 1u;
    if (gw_brokersel_failed(&s_bsel)) {
        ESP_LOGW(TAG, "broker %u/%u (%s:%u) failed %u time(s) in a row: next attempt on broker "
                      "%u/%u (%s:%u)",
                 from + 1, s_bsel.count, broker_host(from), broker_port(from), fails,
                 s_bsel.current + 1, s_bsel.count, broker_host(s_bsel.current),
                 broker_port(s_bsel.current));
        s_attempt = 0;   /* another broker: fresh backoff */
    }
    uint32_t round = (uint32_t)s_bsel.count * s_bsel.failover_after;
    if (gw_brokersel_round_failed(&s_bsel) && s_bsel.round_failures % round == 0 &&
        s_client_if[0] != '\0') {
        ESP_LOGW(TAG, "no broker reachable over %s (%u failed attempts)", s_client_if,
                 (unsigned)s_bsel.round_failures);
        gw_net_report_broker(s_client_if, false);
    }
    publish_status();
}

/* Requested restart: the same orderly end as NCMD Reboot (explicit NDEATH, DISCONNECT). */
static void restart_now(void)
{
    if (s_restart_backstop != NULL) {
        esp_timer_stop(s_restart_backstop);   /* this task answered; each step below is bounded */
    }
    ESP_LOGW(TAG, "restart requested%s", s_node->online ? ": publishing NDEATH" : "");
    spill_before_restart();
    gw_fw_restart_requested();   /* issue #108 B5 */
    if (s_node->online) {
        publish_ndeath();
        op_reboot(NULL);
    }
    gw_bus_quiesce(QUIESCE_MS);
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();
}

/* MQTT-4: TLS settings that cannot work (a missing or unreadable certificate) never connect;
 * and with certificate dates checked (CONFIG_MBEDTLS_HAVE_TIME_DATE) a TLS connection waits
 * for SNTP: without a valid clock every certificate would be "not valid yet". */
static bool tls_can_start(int64_t now)
{
    if (s_setup_error[0] != '\0') {
        if (s_tls_logged_us == 0 || now - s_tls_logged_us >= TLS_ERROR_LOG_MS * 1000LL) {
            s_tls_logged_us = now;
            ESP_LOGE(TAG, "%s: MQTT not started", s_setup_error);
        }
        return false;
    }
#if CONFIG_MBEDTLS_HAVE_TIME_DATE
    if (s_cfg->mqtt.tls.enabled && !time_ok()) {
        if (!s_waiting_time_logged) {
            s_waiting_time_logged = true;
            ESP_LOGW(TAG, "TLS: waiting for SNTP before connecting (certificate dates are checked)");
        }
        return false;
    }
#endif
    return true;
}

/* #124: the card work (the boot recovery first) waits for the network, not to hold up the
 * boot, or for SPILL_GO_WAIT_MS when it does not come. */
static bool spill_wants_time(void)
{
    return s_spill_on && s_spill_go && gw_sf_busy(&s_sf);
}

/* #124: at most one bounded card operation per loop (each one followed by a watchdog feed,
 * through s_spill->tick): the recovery, a superblock, an extent, a block of the RAM ring, or a
 * delete step. Not inlined: its log arguments and card statistics must not grow the frame of
 * reconcile(), which is on the deepest stack path (the history replay). */
static __attribute__((noinline)) void spill_step(int64_t now)
{
    if (!s_spill_on) {
        return;
    }
    if (atomic_load(&s_card_release) && !atomic_load(&s_card_released)) {
        /* a format needs the card: the spill stops for good (until the restart) */
        spill_checkpoint();
        gw_sfspill_stop(s_spill, "stopped: the card was formatted");
        gw_sfcard_close();
        atomic_store(&s_card_released, true);
        ESP_LOGW(TAG, "store & forward: the card is released for a format; the RAM ring goes on");
        return;
    }
    if (!s_spill_go && (atomic_load(&s_ip_up) || now >= SPILL_GO_WAIT_MS * 1000LL)) {
        s_spill_go = true;
        ESP_LOGI(TAG, "store & forward: the card is read now, one step per loop");
    }
    if (!s_spill_go) {
        return;
    }
    gw_sf_step(&s_sf, (uint32_t)(now / 1000));
    if (!s_spill->ok && !s_spill_fail_logged) {
        s_spill_fail_logged = true;
        ESP_LOGE(TAG, "store & forward: spill FAILED (%s): %lu samples stay on the card unsent; "
                      "the RAM ring overwrites its oldest samples when full",
                 s_spill->why, (unsigned long)s_spill->stranded);
        return;
    }
    if (s_spill->phase == GW_SFSPILL_READY && !s_spill_ready_logged) {
        s_spill_ready_logged = true;
        ESP_LOGI(TAG, "store & forward: card ready in %lu ms (%lu blocks read): %lu samples "
                      "waiting, ring %lu of %lu blocks, nonce %08lx (%s); %lu file(s) to delete",
                 (unsigned long)s_spill->boot_ms, (unsigned long)s_spill->scan_reads,
                 (unsigned long)gw_sfspill_waiting(s_spill),
                 (unsigned long)(s_spill->head - s_spill->tail), (unsigned long)s_spill->nblocks,
                 (unsigned long)s_spill->nonce, gw_sfspill_found_text(s_spill),
                 (unsigned long)gw_sfspill_gc_pending(s_spill));
    }
    if (s_spill->gc_files != s_spill_gc_logged && gw_sfspill_gc_pending(s_spill) == 0) {
        gw_sfcard_stats_t cs;
        gw_sfcard_get_stats(&cs);
        ESP_LOGI(TAG, "store & forward: card: %lu file(s) deleted in %lu steps since boot (longest "
                      "step %lu ms), %lu left for the next boot",
                 (unsigned long)s_spill->gc_files, (unsigned long)s_spill->gc_steps,
                 (unsigned long)(cs.op[GW_SFCARD_STEP].max_us / 1000),
                 (unsigned long)s_spill->gc_left);
        s_spill_gc_logged = s_spill->gc_files;
    }
}

/* Bring the Sparkplug state in line with the flags set by the other tasks. */
static void reconcile(void)
{
    int64_t now = esp_timer_get_time();
    if (atomic_load(&s_restart_req)) {
        restart_now();
    }
    bool ip = atomic_load(&s_ip_up);
    /* The MQTT task stores the flag before it bumps a generation, so the generations must be
     * read before the flag: then a generation is only ever consumed with a flag at least as
     * recent as the event that bumped it. Read the other way round, a CONNECTED landing
     * between the two loads was consumed with connected == false and the session stayed up
     * forever without subscriptions or NBIRTH (and a DISCONNECTED, symmetrically, without a
     * reconnect). An event that lands after these loads is handled on the next loop. */
    unsigned cg = atomic_load(&s_conn_gen);
    unsigned dg = atomic_load(&s_disc_gen);
    bool connected = atomic_load(&s_mqtt_connected);
    if (ip && !s_prev_ip) {
        s_ip_since_us = now;
    }
    s_prev_ip = ip;
    gw_status_set(GW_ST_NET_UP, ip);

    unsigned rg = atomic_load(&s_route_gen);
    if (rg != s_handled_route) {
        s_handled_route = rg;
        if (s_started) {
            move_session("active network interface changed");
            gw_brokersel_init(&s_bsel, s_bsel.count, s_bsel.failover_after,
                              s_bsel.primary_return_ms);   /* a new path: the primary first */
            /* move_session consumed the generations of the old client: nothing of it is
             * handled below (no on_connected, no retry: the new client starts at once). */
            connected = false;
            cg = s_handled_conn;
            dg = s_handled_disc;
        }
    }

    if (cg != s_handled_conn) {
        s_handled_conn = cg;
        s_sub_msg_id = 0;
        if (connected) {
            on_connected();
        }
    }
    if (s_sub_msg_id > 0) {
        int ack = atomic_load(&s_suback);
        if (!connected) {
            s_sub_msg_id = 0;
        } else if (ack == s_sub_msg_id) {
            s_sub_msg_id = 0;
            on_subscribed();
        } else if (ack == -s_sub_msg_id) {
            abort_session(s_state_topic[0] ? "the broker refused the NCMD/DCMD/STATE subscriptions"
                                           : "the broker refused the NCMD/DCMD subscriptions");
        } else if (now >= s_sub_deadline_us) {
            abort_session(s_state_topic[0]
                              ? "no SUBACK for NCMD/DCMD/STATE within " STR(SUBACK_TIMEOUT_MS) " ms"
                              : "no SUBACK for NCMD/DCMD within " STR(SUBACK_TIMEOUT_MS) " ms");
        }
    }
    if (s_waiting_host && connected && s_bsel.count > 1 &&
        now - s_host_wait_since_us >= PHOST_WAIT_MS * 1000LL) {
        s_host_disconnect = true;
        s_waiting_host = false;
        abort_session("no STATE online from the primary host within 30 s on this broker");
    }
    if (s_node->online && connected && !s_stable && now - s_session_up_us >= stable_ms() * 1000LL) {
        s_stable = true;
        if (s_attempt != 0) {
            ESP_LOGI(TAG, "session stable for %lu ms: reconnect backoff reset",
                     (unsigned long)stable_ms());
            s_attempt = 0;
        }
        gw_brokersel_stable(&s_bsel, (uint64_t)(now / 1000));
        if (s_bsel.count > 1) {
            ESP_LOGI(TAG, "broker %u/%u: session stable, the broker is kept", s_bsel.current + 1,
                     s_bsel.count);
        }
        gw_net_report_broker(s_client_if, true);
        publish_status();
    }
    if (s_stable && connected && gw_brokersel_should_return(&s_bsel, (uint64_t)(now / 1000))) {
        /* MQTT-6: back to the primary after primary_return_s on a backup (one attempt) */
        gw_brokersel_return_to_primary(&s_bsel);
        move_session("returning to the primary broker");
        connected = false;
        cg = s_handled_conn;
        dg = s_handled_disc;
    }
    if (s_node->online && !connected) {
        gw_node_session_down(s_node);
        gw_status_set(GW_ST_MQTT_UP, false);
    }
    if (dg != s_handled_disc) {
        s_handled_disc = dg;
        /* esp-mqtt may report one dead connection twice (the failed write of a DBIRTH, then the
         * reader): one attempt ends once, so the backoff and the broker failover count it once */
        if (!connected && s_retry_at_us == 0) {
            attempt_ended();
            schedule_retry(now);
        }
    }

    if (!s_started && ip && (s_retry_at_us == 0 || now >= s_retry_at_us) && tls_can_start(now) &&
        (time_ok() || now - s_ip_since_us >= CONFIG_GW_SNTP_WAIT_MS * 1000LL)) {
        start_client(now);
    }
    if (s_started && s_retry_at_us != 0 && now >= s_retry_at_us && ip && !connected) {
        reconnect(now);
    }

    if (s_node->online && !s_births_timed && atomic_load(&s_time_synced)) {
        /* (births with the RTC's time were valid: no rebirth for those) */
        /* Births went out before SNTP: re-issue them so hosts get valid timestamps. */
        s_births_timed = true;
        ESP_LOGI(TAG, "clock now synchronised: rebirth");
        gw_node_rebirth(s_node);
    }
    /* TIME-3: NDATA Time Synced when SNTP synchronises after births timed by the RTC (the
     * rebirth above already carried it otherwise) */
    gw_node_set_time_synced(s_node, atomic_load(&s_time_synced));
    sync_firmware();
    if (atomic_exchange(&s_rebirth_req, false) && s_node->online) {
        ESP_LOGI(TAG, "rebirth requested from the shell or the REST API");
        gw_node_rebirth(s_node);
    }
    /* online: DDATA/NDATA; offline with store & forward: the same changes into the store */
    if (s_node->online || s_node->sf != NULL) {
        unsigned dirty = atomic_exchange(&s_dev_dirty, 0);
        for (uint16_t d = 0; dirty != 0; d++, dirty >>= 1) {
            if (dirty & 1u) {
                gw_node_device_changed(s_node, d);
            }
        }
        if (atomic_exchange(&s_io_dirty, false)) {
            gw_node_io_changed(s_node);
        }
    }
    if (s_node->online && gw_sf_count(&s_sf) > 0) {
        /* SPB-14: history after the births, a few messages per loop */
        if (gw_node_flush_history(s_node, SF_MSGS_PER_LOOP) == 0) {
            ESP_LOGI(TAG, "store & forward: history sent (%lu historical messages in total, %lu "
                          "samples of devices without DBIRTH dropped)",
                     (unsigned long)s_node->sf_sent, (unsigned long)s_node->sf_discarded);
        }
    }
    spill_step(now);
    /* SPB-17: periodic diagnostics NDATA (rate limited by sparkplug.diagnostics_s) */
    s_diag_wait_ms = gw_node_diag_tick(s_node, (uint64_t)(esp_timer_get_time() / 1000));
    publish_status();
}

static TickType_t next_timeout(void)
{
    int64_t ms = (s_node->online && gw_sf_count(&s_sf) > 0) || spill_wants_time() ? 10
                                                                                 : MAX_IDLE_MS;
    int64_t now = esp_timer_get_time();
    if (s_sub_msg_id > 0) {
        int64_t r = (s_sub_deadline_us - now) / 1000;
        ms = r < ms ? r : ms;
    }
    if (s_retry_at_us != 0) {
        int64_t r = (s_retry_at_us - now) / 1000;
        ms = r < ms ? r : ms;
    }
    ms = (int64_t)s_diag_wait_ms < ms ? (int64_t)s_diag_wait_ms : ms;
    if (!s_started && s_prev_ip && s_retry_at_us == 0) {
        int64_t r = (s_ip_since_us + CONFIG_GW_SNTP_WAIT_MS * 1000LL - now) / 1000;
        ms = r < ms ? r : ms;
    }
    return ms > 0 ? pdMS_TO_TICKS(ms) + 1 : 1;
}

static void edge_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        edge_ev_t ev;
        bool got = xQueueReceive(s_q, &ev, next_timeout()) == pdTRUE;
        feed_watchdog();
        reconcile();
        if (!got) {
            continue;
        }
        if (ev.type == EV_CMD) {
            const cmd_slot_t *c = &s_slots[ev.arg];
            if (s_state_topic[0] != '\0' && c->topic_len == strlen(s_state_topic) &&
                memcmp(c->topic, s_state_topic, c->topic_len) == 0) {
                on_host_state(c->data, c->len);
            } else {
                gw_node_handle_message(s_node, c->topic, c->topic_len, c->data, c->len);
            }
            uint8_t slot = (uint8_t)ev.arg;
            xQueueSend(s_free_slots, &slot, 0);
        } else if (ev.type == EV_WRITE_DONE) {
            gw_node_write_done(s_node, ev.arg, ev.ok);
        }
    }
}

/* A PEM file next to config.json, checked now so a bad file is reported as such and not as a
 * handshake failure on every attempt. Records the problem in s_setup_error. */
static char *load_pem_certs(const char *name, const char *key)
{
    size_t len;
    char why[120];
    char *pem = gw_storage_read_file(name, &len, why, sizeof(why));
    if (pem == NULL) {
        snprintf(s_setup_error, sizeof(s_setup_error), "mqtt.tls.%s: %s", key, why);
        return NULL;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int r = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, len + 1);
    unsigned count = 0;
    for (const mbedtls_x509_crt *c = &crt; c != NULL && c->raw.len > 0; c = c->next) {
        count++;
    }
    mbedtls_x509_crt_free(&crt);
    if (r != 0 || count == 0) {
        snprintf(s_setup_error, sizeof(s_setup_error),
                 "mqtt.tls.%s: /cfg/%s is not a PEM certificate file (mbedTLS %d)", key, name, r);
        gw_storage_free(pem);
        return NULL;
    }
    ESP_LOGI(TAG, "TLS: %s: %u certificate(s) from /cfg/%s", key, count, name);
    return pem;
}

/* MQTT-5: the client's private key, checked now like the certificates. */
static char *load_pem_key(const char *name)
{
    size_t len;
    char why[120];
    char *pem = gw_storage_read_file(name, &len, why, sizeof(why));
    if (pem == NULL) {
        snprintf(s_setup_error, sizeof(s_setup_error), "mqtt.tls.key_file: %s", why);
        return NULL;
    }
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int r = mbedtls_pk_parse_key(&pk, (const unsigned char *)pem, len + 1, NULL, 0);
    mbedtls_pk_free(&pk);
    if (r != 0) {
        snprintf(s_setup_error, sizeof(s_setup_error),
                 "mqtt.tls.key_file: /cfg/%s is not an unencrypted PEM private key (mbedTLS -0x%04x)",
                 name, (unsigned)-r);
        gw_storage_free(pem);
        return NULL;
    }
    ESP_LOGI(TAG, "TLS: client key from /cfg/%s", name);
    return pem;
}

/* MQTT-4: TLS 1.2/1.3 with the broker certificate verified against the CA file (or the
 * ESP-IDF bundle of public roots) and its name against the host (or tls.server_name). The
 * PEM buffers are referenced, not copied, by esp-mqtt: they live for the whole run. */
static void setup_tls(const gw_tls_cfg_t *tls)
{
    psa_crypto_init();   /* mbedTLS 4: certificate parsing goes through PSA; idempotent */
    esp_mqtt_client_config_t *m = &s_mqtt_cfg;
    if (tls->ca_file[0] != '\0') {
        s_ca_pem = load_pem_certs(tls->ca_file, "ca_file");
        /* parsed once and handed to mbedTLS by tls_attach(), with the verify callback */
        mbedtls_x509_crt_init(&s_ca_chain);
        if (s_ca_pem != NULL) {
            mbedtls_x509_crt_parse(&s_ca_chain, (const unsigned char *)s_ca_pem,
                                   strlen(s_ca_pem) + 1);
        }
    } else {
        ESP_LOGI(TAG, "TLS: broker certificate checked against the ESP-IDF root CA bundle");
    }
    /* esp-tls takes the CA from this hook before cacert_buf or the global store: both cases go
     * through it to install the verify callback of issue #70 */
    m->broker.verification.crt_bundle_attach = tls_attach;
    /* Never skip_cert_common_name_check: the name is always verified (MQTT-4). */
    m->broker.verification.common_name = tls->server_name[0] ? tls->server_name : NULL;
    if (tls->cert_file[0] != '\0' && s_setup_error[0] == '\0') {
        /* MQTT-5: mutual TLS; the broker checks this certificate against its own CA */
        s_cert_pem = load_pem_certs(tls->cert_file, "cert_file");
        s_key_pem = s_cert_pem != NULL ? load_pem_key(tls->key_file) : NULL;
        m->credentials.authentication.certificate = s_cert_pem;
        m->credentials.authentication.key = s_key_pem;
    }
}

/* SPB-14: the optional spill of the ring to the microSD card (gw_sfspill, #124). It needs the
 * PSRAM: the target board has 8 MB, and the spill is not designed for a board without it. */
static void setup_spill(void)
{
    if (!s_sf_psram) {
        snprintf(s_spill_off, sizeof(s_spill_off), "off: it needs PSRAM, and none was detected");
        gw_sfcard_unchecked("the spill needs PSRAM, and none was detected: the boot did not "
                            "look for a card");
        ESP_LOGW(TAG, "store & forward: microSD spill not started: it needs PSRAM, and none was "
                      "detected; the RAM ring overwrites its oldest samples when full");
        return;
    }
#if CONFIG_GW_SF_SPILL_SDMMC
    ESP_LOGW(TAG, "store & forward: microSD enabled. GPIO45 (its D0) is the VDD_SPI strapping "
                  "pin: a card that holds it high at reset can select 1.8 V for the flash and "
                  "stop the board from booting (README 6.5)");
#endif
    static gw_sfspill_io_t io;
    char where[96], why[80];
    if (!gw_sfcard_open(&io, where, sizeof(where), why, sizeof(why), release_card)) {
        snprintf(s_spill_off, sizeof(s_spill_off), "off: %s", why);
        ESP_LOGE(TAG, "store & forward: %s: the RAM ring overwrites its oldest samples when full",
                 why);
        return;
    }
    /* its state, two blocks and the delete queue: allocated once */
    s_spill = gw_storage_big_alloc(sizeof(*s_spill), "store & forward spill state");
    void *mem = s_spill != NULL ? gw_storage_big_alloc(gw_sfspill_mem_size(),
                                                       "store & forward spill buffers")
                                : NULL;
    if (mem == NULL) {
        snprintf(s_spill_off, sizeof(s_spill_off), "off: no memory for its buffers");
        gw_sfcard_close();   /* the card is free for a format */
        return;
    }
    /* extents of min(4 MiB, 128 clusters) of the card, at most 1024 of them */
    uint32_t ext_blocks, extents;
    s_spill_kb = gw_sfspill_plan(s_cfg->sparkplug.sf_sd_max_kb, io.unit, &ext_blocks, &extents);
    /* #145: the records keep their meaning while the fields of this hash do not change */
    uint32_t hash = gw_sfspill_cfg_hash(s_cfg);
    gw_sfspill_init(s_spill, &io, ext_blocks, extents, hash, mem);
    s_spill->tick = feed_watchdog;   /* from now on only the Sparkplug task uses the card */
    s_spill->random = esp_random;
    gw_sf_attach_spill(&s_sf, s_spill);
    s_spill_on = true;
    ESP_LOGI(TAG, "store & forward: spill to %s: a ring of %lu KB, %lu extents of %lu KB, "
                  "configuration %08lx; the card is read after the network starts",
             where, (unsigned long)s_spill_kb, (unsigned long)extents,
             (unsigned long)(ext_blocks * GW_SFSPILL_BLOCK / 1024), (unsigned long)hash);
    if (s_spill_kb < s_cfg->sparkplug.sf_sd_max_kb) {
        ESP_LOGW(TAG, "store & forward: the ring uses %lu KB of the %lu KB of max_kb: at most %u "
                      "extents of %lu clusters of %lu B; format the card with the gateway (web "
                      "UI, or shell: card format confirm) to use more",
                 (unsigned long)s_spill_kb, (unsigned long)s_cfg->sparkplug.sf_sd_max_kb,
                 GW_SFSPILL_MAX_EXTENTS, (unsigned long)(io.unit ? ext_blocks * GW_SFSPILL_BLOCK / io.unit : 0),
                 (unsigned long)io.unit);
    }
}

/* SPB-14: the ring, once at boot: large in PSRAM, small in internal RAM without it. */
static void setup_store(void)
{
    size_t n = CONFIG_GW_SF_PSRAM_SAMPLES;
    gw_sf_rec_t *buf = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0
                           ? heap_caps_calloc(n, sizeof(gw_sf_rec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                           : NULL;
    s_sf_psram = buf != NULL;
    if (buf == NULL) {
        n = CONFIG_GW_SF_INTERNAL_SAMPLES;
        buf = heap_caps_calloc(n, sizeof(gw_sf_rec_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        ESP_LOGE(TAG, "store & forward: no memory for %u samples: changes while offline are "
                      "coalesced",
                 (unsigned)n);
        return;
    }
    gw_sf_init(&s_sf, buf, n);
    gw_node_set_store(s_node, &s_sf);
    ESP_LOGI(TAG, "store & forward: %u samples (%u B) in %s", (unsigned)n,
             (unsigned)(n * sizeof(gw_sf_rec_t)), s_sf_psram ? "PSRAM" : "internal RAM (no PSRAM)");
    if (s_cfg->sparkplug.sf_sd) {
        setup_spill();
    }
}

esp_err_t gw_edge_start(const gw_config_t *cfg, const gw_node_info_t *info)
{
    s_cfg = cfg;
    gw_io_rtc_info_t rtc;
    gw_io_rtc_get(&rtc);
    atomic_store(&s_time_valid, rtc.restored);   /* TIME-2: no SNTP wait with the RTC's time */
#if !CONFIG_MQTT_PROTOCOL_5
    if (cfg->mqtt.protocol == GW_MQTT_V5) {
        ESP_LOGE(TAG, "mqtt.protocol 5 needs CONFIG_MQTT_PROTOCOL_5: MQTT not started");
        snprintf(s_setup_error, sizeof(s_setup_error), "mqtt.protocol: MQTT 5 not in this build");
    }
#endif
    s_node = gw_storage_big_alloc(sizeof(*s_node), "Sparkplug engine");
    if (s_node == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const gw_node_ops_t ops = {
        .publish = op_publish,
        .set_do = op_set_do,
        .request_write = op_request_write,
        .write_room = op_write_room,
        .set_scan_rate = op_set_scan_rate,
        .reboot = op_reboot,
        .firmware_update = op_firmware_update,
        .diagnostics = op_diagnostics,
        .ctx = NULL,
    };
    gw_node_init(s_node, cfg, &ops, info);
    atomic_store(&s_fw_dirty, true);   /* the first NBIRTH carries the update's state */
    /* The largest births (SPB-16/17/18) need GW_SPB_PAYLOAD_MAX: in PSRAM when the board has
     * it; without PSRAM a smaller buffer leaves internal RAM for TLS and the web server. */
    size_t cap = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 ? GW_SPB_PAYLOAD_MAX
                                                                 : CONFIG_GW_SPB_PAYLOAD_INTERNAL;
    uint8_t *buf = gw_storage_big_alloc(cap, "Sparkplug payload buffer");
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    gw_node_set_buffer(s_node, buf, cap);
    if (!gw_node_topic(s_node, "NDEATH", NULL, s_death_topic, sizeof(s_death_topic)) ||
        !gw_node_topic(s_node, "NCMD", NULL, s_ncmd_topic, sizeof(s_ncmd_topic)) ||
        !gw_node_topic(s_node, "DCMD", "+", s_dcmd_topic, sizeof(s_dcmd_topic))) {
        return ESP_ERR_INVALID_SIZE;
    }
    gw_phost_init(&s_phost);
    if (cfg->sparkplug.store_forward) {
        setup_store();
    }
    if (cfg->sparkplug.primary_host_id[0] != '\0') {
        snprintf(s_state_topic, sizeof(s_state_topic), "spBv1.0/STATE/%s",
                 cfg->sparkplug.primary_host_id);   /* fits: ids are at most 32 characters */
        ESP_LOGI(TAG, "primary host '%s': NBIRTH only while it is online (%s)",
                 cfg->sparkplug.primary_host_id, s_state_topic);
    }

    s_mqtt_cfg = (esp_mqtt_client_config_t){
        .broker.address = {
            .hostname = cfg->mqtt.host,       /* resolved by DNS on every attempt (NET-3) */
            .port = cfg->mqtt.port,
            .transport = cfg->mqtt.tls.enabled ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP,
        },
        .credentials = {
            .client_id = cfg->mqtt.client_id,
            .username = cfg->mqtt.username[0] ? cfg->mqtt.username : NULL,
            .authentication.password = cfg->mqtt.password[0] ? cfg->mqtt.password : NULL,
        },
        .session = {
            .protocol_ver = cfg->mqtt.protocol == GW_MQTT_V5 ? MQTT_PROTOCOL_V_5 : MQTT_PROTOCOL_V_3_1_1,
            .keepalive = cfg->mqtt.keepalive_s,
            .last_will = {.topic = s_death_topic, .qos = 1, .retain = 0},
        },
        .network = {
            .disable_auto_reconnect = true,   /* backoff and bdSeq handled here */
            .timeout_ms = NET_TIMEOUT_MS,
        },
        .buffer = {.size = CONFIG_GW_MQTT_BUFFER_SIZE, .out_size = CONFIG_GW_MQTT_BUFFER_SIZE},
        .outbox = {.limit = CONFIG_GW_MQTT_OUTBOX_LIMIT},
    };

    if (cfg->mqtt.tls.enabled) {
        setup_tls(&cfg->mqtt.tls);
    }
    gw_brokersel_init(&s_bsel, (uint8_t)(1 + cfg->mqtt.backup_count), cfg->mqtt.failover_after,
                      cfg->mqtt.primary_return_s * 1000u);
    publish_status();

    s_pub_lock = xSemaphoreCreateMutexStatic(&s_pub_lock_buf);
    const esp_timer_create_args_t pt = {.callback = on_publish_budget, .name = "pub_budget"};
    ESP_ERROR_CHECK(esp_timer_create(&pt, &s_pub_timer));
    const esp_timer_create_args_t rb = {.callback = on_restart_backstop, .name = "restart_bound"};
    ESP_ERROR_CHECK(esp_timer_create(&rb, &s_restart_backstop));
    s_q = xQueueCreateStatic(CONFIG_GW_EDGE_QUEUE_LEN, sizeof(edge_ev_t), s_q_storage, &s_q_buf);
    s_free_slots = xQueueCreateStatic(CONFIG_GW_CMD_SLOTS, 1, s_free_storage, &s_free_buf);
    for (uint8_t i = 0; i < CONFIG_GW_CMD_SLOTS; i++) {
        xQueueSend(s_free_slots, &i, 0);
    }
    TaskHandle_t t = xTaskCreateStaticPinnedToCore(edge_task, "sparkplug",
                                                   STACK_BYTES / sizeof(StackType_t), NULL,
                                                   CONFIG_GW_EDGE_TASK_PRIORITY, s_stack,
                                                   &s_task_buf, CONFIG_GW_EDGE_TASK_CORE);
    return t != NULL ? ESP_OK : ESP_FAIL;
}
