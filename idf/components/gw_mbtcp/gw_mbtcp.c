#include "gw_mbtcp.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw_bus.h"
#include "gw_mbap.h"
#include "lwip/sockets.h"

static const char *TAG = "mbtcp";

#define STACK_WORDS (4096 / sizeof(StackType_t))
#define SELECT_MS   1000      /* wake at least this often to expire idle clients */
#define SEND_TIMEOUT_S 2      /* a client that does not read its answers is dropped */

typedef struct {
    int fd;                   /* -1: free */
    uint8_t ip[4];
    size_t have;
    int64_t last_us;
    uint8_t buf[GW_MBAP_ADU_MAX];
} client_t;

static const gw_config_t *s_cfg;
static client_t s_clients[CONFIG_GW_MBTCP_MAX_CLIENTS];
static gw_mbap_txn_t s_txn;   /* one transaction at a time, shared with the bus owner */
static uint8_t s_out[GW_MBAP_ADU_MAX];
static StaticTask_t s_task_buf;
static StackType_t s_stack[STACK_WORDS];

static int64_t now_us(void)
{
    return (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS * 1000;
}

static void drop(client_t *c, const char *why)
{
    ESP_LOGI(TAG, "client %u.%u.%u.%u closed: %s", c->ip[0], c->ip[1], c->ip[2], c->ip[3], why);
    close(c->fd);
    c->fd = -1;
    c->have = 0;
}

static bool reply(client_t *c, const uint8_t *data, size_t len)
{
    if (len == 0 || send(c->fd, data, len, 0) != (ssize_t)len) {
        drop(c, "send failed");
        return false;
    }
    return true;
}

/* One complete MBAP frame: answer it (from the slave, or with the gateway's own exception). */
static bool handle_frame(client_t *c, const uint8_t *frame, size_t len)
{
    /* A previous request that timed out may still be running on the bus: s_txn is shared. */
    if (!gw_bus_forward_idle(CONFIG_GW_MBTCP_RESPONSE_WAIT_MS)) {
        gw_mbap_count_exception(GW_MBAP_EX_TARGET_NO_ANSWER);
        return reply(c, s_out, gw_mbap_exception(frame, GW_MBAP_EX_TARGET_NO_ANSWER, s_out,
                                                 sizeof(s_out)));
    }
    uint8_t code = gw_mbap_parse(frame, len, &s_txn);
    if (code != 0) {
        ESP_LOGD(TAG, "unit %u fc %u: answered by the gateway with exception %u", frame[6],
                 frame[GW_MBAP_HDR_LEN], code);
        gw_mbap_count_exception(code);
        return reply(c, s_out, gw_mbap_exception(frame, code, s_out, sizeof(s_out)));
    }
    if (!gw_bus_forward(&s_txn, CONFIG_GW_MBTCP_RESPONSE_WAIT_MS)) {
        ESP_LOGW(TAG, "unit %u fc %u: the bus owner did not run it within %d ms", s_txn.rq.unit,
                 s_txn.rq.fc, CONFIG_GW_MBTCP_RESPONSE_WAIT_MS);
        gw_mbap_count_exception(GW_MBAP_EX_TARGET_NO_ANSWER);
        return reply(c, s_out, gw_mbap_exception(frame, GW_MBAP_EX_TARGET_NO_ANSWER, s_out,
                                                 sizeof(s_out)));
    }
    return reply(c, s_out, gw_mbap_response(&s_txn, s_out, sizeof(s_out)));
}

static void on_readable(client_t *c)
{
    ssize_t n = recv(c->fd, &c->buf[c->have], sizeof(c->buf) - c->have, 0);
    if (n <= 0) {
        drop(c, n == 0 ? "closed by the client" : "receive error");
        return;
    }
    c->have += (size_t)n;
    c->last_us = now_us();
    for (;;) {
        size_t need = gw_mbap_frame_len(c->buf, c->have);
        if (need == SIZE_MAX) {
            drop(c, "not a Modbus TCP frame");   /* the stream cannot be resynchronised */
            return;
        }
        if (need == 0 || need > c->have) {
            return;
        }
        if (!handle_frame(c, c->buf, need)) {
            return;
        }
        memmove(c->buf, &c->buf[need], c->have - need);
        c->have -= need;
    }
}

static void on_accept(int lfd)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) {
        return;
    }
    uint8_t ip[4];
    memcpy(ip, &peer.sin_addr.s_addr, 4);   /* network order: a.b.c.d */
    if (!gw_mbap_client_allowed(s_cfg, ip)) {
        ESP_LOGW(TAG, "connection from %u.%u.%u.%u refused: not in modbus_tcp.allowed_clients",
                 ip[0], ip[1], ip[2], ip[3]);
        close(fd);
        gw_mbap_stats_add(&(const gw_mbap_stats_t){.rejected = 1});
        return;
    }
    client_t *c = NULL;
    for (int i = 0; i < CONFIG_GW_MBTCP_MAX_CLIENTS && c == NULL; i++) {
        c = s_clients[i].fd < 0 ? &s_clients[i] : NULL;
    }
    if (c == NULL) {
        ESP_LOGW(TAG, "connection from %u.%u.%u.%u refused: %d clients already connected", ip[0],
                 ip[1], ip[2], ip[3], CONFIG_GW_MBTCP_MAX_CLIENTS);
        close(fd);
        gw_mbap_stats_add(&(const gw_mbap_stats_t){.rejected = 1});
        return;
    }
    struct timeval tv = {.tv_sec = SEND_TIMEOUT_S};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    c->fd = fd;
    memcpy(c->ip, ip, 4);
    c->have = 0;
    c->last_us = now_us();
    gw_mbap_stats_add(&(const gw_mbap_stats_t){.accepted = 1});
    ESP_LOGI(TAG, "client %u.%u.%u.%u connected", ip[0], ip[1], ip[2], ip[3]);
}

static int open_listener(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(s_cfg->modbus_tcp.port),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void server_task(void *arg)
{
    (void)arg;
    int lfd;
    while ((lfd = open_listener()) < 0) {
        ESP_LOGE(TAG, "cannot listen on TCP port %u; retrying in 5 s", s_cfg->modbus_tcp.port);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI(TAG, "Modbus TCP gateway listening on port %u (%u allowed client entries%s)",
             s_cfg->modbus_tcp.port, s_cfg->modbus_tcp.client_count,
             s_cfg->modbus_tcp.client_count == 0 ? ": any client" : "");
    const int64_t idle_us = (int64_t)s_cfg->modbus_tcp.idle_timeout_s * 1000000;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(lfd, &rd);
        int maxfd = lfd;
        for (int i = 0; i < CONFIG_GW_MBTCP_MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0) {
                FD_SET(s_clients[i].fd, &rd);
                maxfd = s_clients[i].fd > maxfd ? s_clients[i].fd : maxfd;
            }
        }
        struct timeval tv = {.tv_sec = SELECT_MS / 1000};
        int n = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (n > 0 && FD_ISSET(lfd, &rd)) {
            on_accept(lfd);
        }
        int64_t now = now_us();
        for (int i = 0; i < CONFIG_GW_MBTCP_MAX_CLIENTS; i++) {
            client_t *c = &s_clients[i];
            if (c->fd < 0) {
                continue;
            }
            if (n > 0 && FD_ISSET(c->fd, &rd)) {
                on_readable(c);
            } else if (idle_us > 0 && now - c->last_us > idle_us) {
                drop(c, "idle");
            }
        }
    }
}

esp_err_t gw_mbtcp_start(const gw_config_t *cfg)
{
    if (!cfg->modbus_tcp.enabled) {
        return ESP_OK;
    }
    s_cfg = cfg;
    for (int i = 0; i < CONFIG_GW_MBTCP_MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
    }
    if (cfg->modbus_tcp.client_count == 0) {
        ESP_LOGW(TAG, "modbus_tcp.allowed_clients is empty: any host that reaches port %u can "
                      "read and write every RS485 slave (Modbus TCP has no authentication)",
                 cfg->modbus_tcp.port);
    }
    TaskHandle_t t = xTaskCreateStaticPinnedToCore(server_task, "modbus_tcp", STACK_WORDS, NULL,
                                                   CONFIG_GW_MBTCP_TASK_PRIORITY, s_stack,
                                                   &s_task_buf, 0);
    return t != NULL ? ESP_OK : ESP_FAIL;
}
