#include "gw_edge.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gw_backoff.h"
#include "gw_bus.h"
#include "gw_io.h"
#include "gw_net.h"
#include "gw_node.h"
#include "gw_status.h"
#include "gw_storage.h"
#include "lwip/sockets.h"
#include "mbedtls/x509_crt.h"
#include "mqtt_client.h"
#include "psa/crypto.h"

static const char *TAG = "edge";

#define STACK_BYTES      6144
#define MAX_IDLE_MS      1000
#define NET_TIMEOUT_MS   5000   /* esp-mqtt socket timeout (one socket write) */
#define REBOOT_DELAY_MS  100    /* let the log drain */
#define QUIESCE_MS       3000   /* longest wait for the bus task to finish its transaction */
#define STABLE_MAX_MS    60000  /* a session is stable after one keepalive period, at most this */
#define SUBACK_TIMEOUT_MS 5000  /* NBIRTH waits this long at most for the SUBACK */
#define TLS_ERROR_LOG_MS 10000  /* a TLS setup that cannot work is logged this often */
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
static atomic_bool s_mqtt_connected;
static atomic_bool s_io_dirty;
static atomic_uint s_conn_gen;
static atomic_uint s_disc_gen;
static atomic_uint s_dev_dirty;
static atomic_int s_suback;     /* msg_id of the last SUBACK, negated if a topic was refused */
static atomic_uint s_route_gen;
static atomic_bool s_restart_req;
static atomic_bool s_rebirth_req;

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
static gw_node_t *s_node;   /* ~27 KB, in PSRAM when present (gw_storage_big_alloc) */
static esp_mqtt_client_handle_t s_client;
static esp_mqtt_client_config_t s_mqtt_cfg;
static char s_death_topic[GW_TOPIC_MAX];
static char s_ncmd_topic[GW_TOPIC_MAX];
static char s_dcmd_topic[GW_TOPIC_MAX];
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
static unsigned s_handled_route;
static struct ifreq s_ifr;      /* interface the client is bound to (NET-4, two interfaces only) */
static char *s_ca_pem;          /* MQTT-4: CA file, boot allocation (PSRAM if any), never freed */
static char s_tls_error[200];   /* why the TLS settings cannot work: no connection is attempted */
static int64_t s_tls_logged_us;
static bool s_waiting_time_logged;

static void wake(void)
{
    const edge_ev_t ev = {.type = EV_WAKE};
    xQueueSend(s_q, &ev, 0);  /* dropping is harmless: state lives in the flags */
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

void gw_edge_request_restart(void)
{
    atomic_store(&s_restart_req, true);
    wake();
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

/* MQTT-4: say why a TLS connection failed, in terms of the configuration. esp-tls reports the
 * verification flags only when the peer certificate is still around after the failure (not
 * with TLS 1.3): then only "verification failed" is known. */
static void log_tls_error(const esp_mqtt_error_codes_t *e)
{
    unsigned flags = (unsigned)e->esp_tls_cert_verify_flags;
    int stack = e->esp_tls_stack_err < 0 ? -e->esp_tls_stack_err : e->esp_tls_stack_err;
    const char *what = (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
                           ? "the broker certificate does not carry the expected name "
                             "(mqtt.host or mqtt.tls.server_name)"
                       : (flags & MBEDTLS_X509_BADCERT_EXPIRED) ? "the broker certificate expired"
                       : (flags & MBEDTLS_X509_BADCERT_FUTURE)
                           ? "the broker certificate is not valid yet (check the clock)"
                       : (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
                           ? "the broker certificate is not signed by the configured CA"
                       : flags != 0 ? "the broker certificate was rejected"
                       : stack == -MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
                           ? "the broker certificate failed verification (CA: mqtt.tls.ca_file; "
                             "name: mqtt.host or mqtt.tls.server_name; dates)"
                           : "TLS handshake failed";
    ESP_LOGW(TAG, "TLS: %s (esp-tls %s, mbedTLS -0x%04x, verify flags 0x%x, errno %d)", what,
             esp_err_to_name(e->esp_tls_last_esp_err), (unsigned)stack, flags,
             e->esp_transport_sock_errno);
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_t *ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
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
        } else if (ev->error_handle->esp_tls_last_esp_err != ESP_OK ||
                   ev->error_handle->esp_tls_cert_verify_flags != 0) {
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
    }
    s_fed_us = now;
    esp_task_wdt_reset();
}

/* ---------- node ops (run in the Sparkplug task) ---------- */

static int op_publish(void *ctx, const char *topic, const uint8_t *data, size_t len, int qos,
                      bool retain)
{
    (void)ctx;
    int r = esp_mqtt_client_publish(s_client, topic, (const char *)data, (int)len, qos, retain);
    feed_watchdog();
    return r;
}

static bool op_set_do(void *ctx, uint8_t index, bool on)
{
    (void)ctx;
    return gw_io_set_do(index, on);
}

static bool op_request_write(void *ctx, uint16_t tag, double value)
{
    (void)ctx;
    return gw_bus_request_write(tag, value);
}

static void op_reboot(void *ctx)
{
    (void)ctx;
    /* Orderly shutdown: NDEATH was just published; stop() sends DISCONNECT synchronously so
     * the broker discards the will instead of publishing a second NDEATH. */
    esp_mqtt_client_stop(s_client);
    /* No Modbus frame cut by the restart (bounded by the slowest transaction with retries). */
    gw_bus_quiesce(QUIESCE_MS);
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    esp_restart();
}

/* ---------- connection management ---------- */

static void prepare_attempt(void)
{
    s_bdseq = gw_storage_next_bdseq();
    size_t n = gw_node_death_payload(s_bdseq, s_will, sizeof(s_will));
    s_mqtt_cfg.session.last_will.msg = (const char *)s_will;
    s_mqtt_cfg.session.last_will.msg_len = (int)n;
    ESP_LOGI(TAG, "connecting to %s:%u%s as '%s', bdSeq=%u", s_cfg->mqtt.host, s_cfg->mqtt.port,
             s_cfg->mqtt.tls.enabled ? " (TLS)" : "", s_cfg->mqtt.client_id, s_bdseq);
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
    if (s_mqtt_cfg.network.if_name != NULL) {
        ESP_LOGI(TAG, "MQTT bound to interface %s", s_ifr.ifr_name);
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

static void reconnect(int64_t now)
{
    s_retry_at_us = 0;
    prepare_attempt();
    if (esp_mqtt_set_config(s_client, &s_mqtt_cfg) != ESP_OK ||
        esp_mqtt_client_reconnect(s_client) != ESP_OK) {
        schedule_retry(now);
    }
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
    const esp_mqtt_topic_t topics[] = {
        {.filter = s_ncmd_topic, .qos = 1},
        {.filter = s_dcmd_topic, .qos = 1},
    };
    atomic_store(&s_suback, 0);
    int id = esp_mqtt_client_subscribe_multiple(s_client, topics, 2);
    feed_watchdog();
    if (id < 0) {
        abort_session("SUBSCRIBE to NCMD/DCMD could not be sent");
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

/* SUBACK received for both topics: the Sparkplug session starts (NBIRTH and DBIRTHs). */
static void on_subscribed(void)
{
    /* s_attempt is not reset here: a broker that accepts the connection and drops it at once
     * (ACL, duplicate client_id, connection limit) must still see the backoff grow. It is
     * reset once the session has stayed up for a while (reconcile). */
    s_retry_at_us = 0;
    atomic_store(&s_dev_dirty, 0);   /* the births carry the current state */
    atomic_store(&s_io_dirty, false);
    s_births_timed = atomic_load(&s_time_synced);
    feed_watchdog();
    s_max_unfed_ms = 0;
    int64_t t0 = esp_timer_get_time();
    gw_node_session_up(s_node, s_bdseq);
    s_session_up_us = esp_timer_get_time();
    gw_status_set(GW_ST_MQTT_UP, true);
    ESP_LOGI(TAG, "Sparkplug session up (bdSeq=%u): births sent in %lu ms, longest stretch "
             "without feeding the watchdog %lu ms", s_bdseq,
             (unsigned long)((esp_timer_get_time() - t0) / 1000), (unsigned long)s_max_unfed_ms);
    log_stacks();
}

/* How long a session must last before the reconnect backoff starts again from the minimum. */
static uint32_t stable_ms(void)
{
    uint32_t ms = (uint32_t)s_cfg->mqtt.keepalive_s * 1000u;
    return ms < STABLE_MAX_MS ? ms : STABLE_MAX_MS;
}

/* NET-4: the active interface changed. A TCP connection cannot migrate, so the session ends
 * here and a new one (new bdSeq, NBIRTH, DBIRTHs) starts over the new interface at once. */
static void move_session(void)
{
    ESP_LOGW(TAG, "active network interface changed: new MQTT session");
    if (s_node->online) {
        /* Orderly end, as for a reboot: hosts learn it now and not after a keepalive. Over a
         * dead interface this is lost and the broker publishes the will later; the bdSeq of
         * the new session tells the two apart. */
        esp_mqtt_client_publish(s_client, s_death_topic, (const char *)s_will,
                                s_mqtt_cfg.session.last_will.msg_len, 1, false);
        feed_watchdog();
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
    s_attempt = 0;   /* a new interface starts a fresh backoff */
    s_sub_msg_id = 0;
}

/* Requested restart: the same orderly end as NCMD Reboot (explicit NDEATH, DISCONNECT). */
static void restart_now(void)
{
    ESP_LOGW(TAG, "restart requested%s", s_node->online ? ": publishing NDEATH" : "");
    if (s_node->online) {
        esp_mqtt_client_publish(s_client, s_death_topic, (const char *)s_will,
                                s_mqtt_cfg.session.last_will.msg_len, 1, false);
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
    if (s_tls_error[0] != '\0') {
        if (s_tls_logged_us == 0 || now - s_tls_logged_us >= TLS_ERROR_LOG_MS * 1000LL) {
            s_tls_logged_us = now;
            ESP_LOGE(TAG, "%s: MQTT not started", s_tls_error);
        }
        return false;
    }
#if CONFIG_MBEDTLS_HAVE_TIME_DATE
    if (s_cfg->mqtt.tls.enabled && !atomic_load(&s_time_synced)) {
        if (!s_waiting_time_logged) {
            s_waiting_time_logged = true;
            ESP_LOGW(TAG, "TLS: waiting for SNTP before connecting (certificate dates are checked)");
        }
        return false;
    }
#endif
    return true;
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
            move_session();
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
            abort_session("the broker refused the NCMD/DCMD subscriptions");
        } else if (now >= s_sub_deadline_us) {
            abort_session("no SUBACK for NCMD/DCMD within " STR(SUBACK_TIMEOUT_MS) " ms");
        }
    }
    if (s_node->online && connected && s_attempt != 0 &&
        now - s_session_up_us >= stable_ms() * 1000LL) {
        ESP_LOGI(TAG, "session stable for %lu ms: reconnect backoff reset",
                 (unsigned long)stable_ms());
        s_attempt = 0;
    }
    if (s_node->online && !connected) {
        gw_node_session_down(s_node);
        gw_status_set(GW_ST_MQTT_UP, false);
    }
    if (dg != s_handled_disc) {
        s_handled_disc = dg;
        if (!connected) {
            schedule_retry(now);
        }
    }

    if (!s_started && ip && (s_retry_at_us == 0 || now >= s_retry_at_us) && tls_can_start(now) &&
        (atomic_load(&s_time_synced) || now - s_ip_since_us >= CONFIG_GW_SNTP_WAIT_MS * 1000LL)) {
        start_client(now);
    }
    if (s_started && s_retry_at_us != 0 && now >= s_retry_at_us && ip && !connected) {
        reconnect(now);
    }

    if (gw_io_outputs_recover()) {
        atomic_store(&s_io_dirty, true);   /* NDATA: the DOs go from null to their state */
    }
    if (s_node->online && !s_births_timed && atomic_load(&s_time_synced)) {
        /* Births went out before SNTP: re-issue them so hosts get valid timestamps. */
        s_births_timed = true;
        ESP_LOGI(TAG, "clock now synchronised: rebirth");
        gw_node_rebirth(s_node);
    }
    if (atomic_exchange(&s_rebirth_req, false) && s_node->online) {
        ESP_LOGI(TAG, "rebirth requested from the console");
        gw_node_rebirth(s_node);
    }
    if (s_node->online) {
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
}

static TickType_t next_timeout(void)
{
    int64_t ms = MAX_IDLE_MS;
    int64_t now = esp_timer_get_time();
    if (s_sub_msg_id > 0) {
        int64_t r = (s_sub_deadline_us - now) / 1000;
        ms = r < ms ? r : ms;
    }
    if (s_retry_at_us != 0) {
        int64_t r = (s_retry_at_us - now) / 1000;
        ms = r < ms ? r : ms;
    }
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
            gw_node_handle_message(s_node, c->topic, c->topic_len, c->data, c->len);
            uint8_t slot = (uint8_t)ev.arg;
            xQueueSend(s_free_slots, &slot, 0);
        } else if (ev.type == EV_WRITE_DONE) {
            gw_node_write_done(s_node, ev.arg, ev.ok);
        }
    }
}

/* A PEM file next to config.json, checked now so a bad file is reported as such and not as a
 * handshake failure on every attempt. Records the problem in s_tls_error. */
static char *load_pem_certs(const char *name, const char *key)
{
    size_t len;
    char why[120];
    char *pem = gw_storage_read_file(name, &len, why, sizeof(why));
    if (pem == NULL) {
        snprintf(s_tls_error, sizeof(s_tls_error), "mqtt.tls.%s: %s", key, why);
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
        snprintf(s_tls_error, sizeof(s_tls_error),
                 "mqtt.tls.%s: /cfg/%s is not a PEM certificate file (mbedTLS %d)", key, name, r);
        gw_storage_free(pem);
        return NULL;
    }
    ESP_LOGI(TAG, "TLS: %s: %u certificate(s) from /cfg/%s", key, count, name);
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
        m->broker.verification.certificate = s_ca_pem;   /* PEM: NUL terminated, length 0 */
    } else {
        m->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        ESP_LOGI(TAG, "TLS: broker certificate checked against the ESP-IDF root CA bundle");
    }
    /* Never skip_cert_common_name_check: the name is always verified (MQTT-4). */
    m->broker.verification.common_name = tls->server_name[0] ? tls->server_name : NULL;
}

esp_err_t gw_edge_start(const gw_config_t *cfg, const gw_node_info_t *info)
{
    s_cfg = cfg;
    s_node = gw_storage_big_alloc(sizeof(*s_node), "Sparkplug engine");
    if (s_node == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const gw_node_ops_t ops = {
        .publish = op_publish,
        .set_do = op_set_do,
        .request_write = op_request_write,
        .reboot = op_reboot,
        .ctx = NULL,
    };
    gw_node_init(s_node, cfg, &ops, info);
    if (!gw_node_topic(s_node, "NDEATH", NULL, s_death_topic, sizeof(s_death_topic)) ||
        !gw_node_topic(s_node, "NCMD", NULL, s_ncmd_topic, sizeof(s_ncmd_topic)) ||
        !gw_node_topic(s_node, "DCMD", "+", s_dcmd_topic, sizeof(s_dcmd_topic))) {
        return ESP_ERR_INVALID_SIZE;
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
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
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
