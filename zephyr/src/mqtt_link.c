/*
 * MQTT session thread. It is the only user of the MQTT client: it connects
 * with the Sparkplug NDEATH as will (MQTT-3, SPB-2), subscribes to the
 * command topics, lets spb_node publish BIRTH/DATA messages and feeds it
 * the incoming commands. It sleeps in poll() on the socket and on an
 * eventfd that the producers (poller, local I/O) signal on state changes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mqtt_link.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/zvfs/eventfd.h>

#include <zephyr/app_version.h>

#include "backoff.h"
#include "bdseq.h"
#include "gw_time.h"
#include "local_io.h"
#include "mb_poller.h"
#include "model.h"
#include "netmgr.h"
#include "sparkplug.h"
#include "spb_node.h"

LOG_MODULE_REGISTER(mqtt_link, CONFIG_GW_LOG_LEVEL);

#define CONNACK_TIMEOUT_MS 5000
#define POLL_MAX_MS        1000 /* producers wake the thread through the eventfd */
#define POLL_FALLBACK_MS   20   /* without an eventfd, poll often enough for IO-1 */
#define SEND_TIMEOUT_S     10
#define TIME_WAIT_S        5 /* first connection: wait this long for SNTP */
#define TOPIC_MAX          128

K_THREAD_STACK_DEFINE(link_stack, 4096);
static struct k_thread link_thread;

static const struct gw_config *cfg;
static struct mqtt_client client;
static struct net_sockaddr_storage broker;
static uint8_t rx_buf[512];
static uint8_t tx_buf[512];
static uint8_t cmd_buf[CONFIG_GW_MQTT_RX_PAYLOAD_SIZE];
static uint8_t death_buf[64];
static size_t death_len;
static char death_topic[TOPIC_MAX];
static char ncmd_topic[TOPIC_MAX];
static char dcmd_topic[TOPIC_MAX];
static struct mqtt_topic will_topic;
static struct mqtt_utf8 will_msg;
static struct mqtt_utf8 user_name;
static struct mqtt_utf8 password;
static uint16_t msg_id;
static int evfd = -1;
static int wdt = -1;

static bool connack_seen;
static int connack_result;
static bool session_lost;

static const char firmware[] = APP_VERSION_STRING "+" STRINGIFY(APP_BUILD_VERSION);

static void feed_wdt(void)
{
	if (wdt >= 0) {
		(void)task_wdt_feed(wdt);
	}
}

/* Producers call this from their own threads. */
static void wake(void)
{
	if (evfd >= 0) {
		(void)zvfs_eventfd_write(evfd, 1);
	}
}

static uint16_t next_msg_id(void)
{
	if (++msg_id == 0) {
		msg_id = 1;
	}
	return msg_id;
}

/* ---- spb_node operations ---------------------------------------------- */

static int link_publish(const char *topic, const uint8_t *payload, size_t len, uint8_t qos)
{
	struct mqtt_publish_param p = {0};

	p.message.topic.topic.utf8 = (const uint8_t *)topic;
	p.message.topic.topic.size = strlen(topic);
	p.message.topic.qos = qos;
	p.message.payload.data = (uint8_t *)payload;
	p.message.payload.len = len;
	p.message_id = (qos > 0) ? next_msg_id() : 0;
	p.retain_flag = 0; /* Sparkplug messages are never retained */
	return mqtt_publish(&client, &p);
}

static const struct spb_node_ops node_ops = {
	.publish = link_publish,
	.write_tag = mb_poller_write,
	.set_output = local_io_set_output,
	.now_ms = gw_time_now_ms,
};

/* ---- MQTT events -------------------------------------------------------- */

static void discard_payload(size_t len)
{
	while (len > 0) {
		int n = mqtt_read_publish_payload_blocking(&client, cmd_buf,
							   MIN(len, sizeof(cmd_buf)));
		if (n <= 0) {
			return;
		}
		len -= (size_t)n;
	}
}

static void on_publish(const struct mqtt_publish_param *p)
{
	char topic[TOPIC_MAX];
	size_t tlen = p->message.topic.topic.size;
	size_t len = p->message.payload.len;
	bool ok = tlen < sizeof(topic);

	if (ok) {
		memcpy(topic, p->message.topic.topic.utf8, tlen);
		topic[tlen] = '\0';
	}
	if (len > sizeof(cmd_buf)) {
		LOG_WRN("command payload of %u bytes exceeds %u, discarded", (unsigned int)len,
			(unsigned int)sizeof(cmd_buf));
		discard_payload(len);
		ok = false;
	} else if (mqtt_readall_publish_payload(&client, cmd_buf, len) < 0) {
		LOG_WRN("cannot read command payload");
		return;
	}
	if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
		const struct mqtt_puback_param ack = {.message_id = p->message_id};

		(void)mqtt_publish_qos1_ack(&client, &ack);
	}
	if (ok) {
		spb_node_on_command(topic, tlen, cmd_buf, len);
	}
}

static void mqtt_evt_handler(struct mqtt_client *c, const struct mqtt_evt *evt)
{
	ARG_UNUSED(c);

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		connack_seen = true;
		connack_result = evt->result;
		break;
	case MQTT_EVT_DISCONNECT:
		session_lost = true;
		break;
	case MQTT_EVT_PUBLISH:
		on_publish(&evt->param.publish);
		break;
	case MQTT_EVT_SUBACK:
		LOG_DBG("SUBACK %u", evt->param.suback.message_id);
		break;
	default:
		break;
	}
}

/* ---- connection --------------------------------------------------------- */

static int resolve_broker(void)
{
	struct zsock_addrinfo hints = {.ai_family = NET_AF_INET, .ai_socktype = NET_SOCK_STREAM};
	struct zsock_addrinfo *res = NULL;
	struct net_sockaddr_in *sin = (struct net_sockaddr_in *)&broker;
	int ret;

	ret = zsock_getaddrinfo(cfg->mqtt.host, NULL, &hints, &res);
	if (ret != 0 || res == NULL) {
		LOG_WRN("cannot resolve %s (%d)", cfg->mqtt.host, ret);
		return -EHOSTUNREACH;
	}
	memcpy(sin, res->ai_addr, sizeof(*sin));
	sin->sin_port = net_htons(cfg->mqtt.port);
	zsock_freeaddrinfo(res);
	return 0;
}

static void prepare_client(uint8_t bdseq)
{
	mqtt_client_init(&client);
	client.broker = &broker;
	client.evt_cb = mqtt_evt_handler;
	client.client_id.utf8 = (const uint8_t *)cfg->mqtt.client_id;
	client.client_id.size = strlen(cfg->mqtt.client_id);
	client.protocol_version = MQTT_VERSION_3_1_1;
	client.clean_session = 1; /* required by Sparkplug 3.0 with MQTT 3.1.1 */
	client.keepalive = cfg->mqtt.keepalive_s;
	client.rx_buf = rx_buf;
	client.rx_buf_size = sizeof(rx_buf);
	client.tx_buf = tx_buf;
	client.tx_buf_size = sizeof(tx_buf);
	client.transport.type = MQTT_TRANSPORT_NON_SECURE;

	if (cfg->mqtt.username[0] != '\0') {
		user_name.utf8 = (const uint8_t *)cfg->mqtt.username;
		user_name.size = strlen(cfg->mqtt.username);
		client.user_name = &user_name;
	}
	if (cfg->mqtt.password[0] != '\0') {
		password.utf8 = (const uint8_t *)cfg->mqtt.password;
		password.size = strlen(cfg->mqtt.password);
		client.password = &password;
	}

	/* The will is this session's NDEATH: QoS 1, not retained (SPB-2). */
	if (spb_node_encode_death(bdseq, death_buf, sizeof(death_buf), &death_len) < 0) {
		death_len = 0;
	}
	will_topic.topic.utf8 = (const uint8_t *)death_topic;
	will_topic.topic.size = strlen(death_topic);
	will_topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	will_msg.utf8 = death_buf;
	will_msg.size = death_len;
	client.will_topic = &will_topic;
	client.will_message = &will_msg;
	client.will_retain = 0;
}

static int poll_socket(int timeout_ms)
{
	struct zsock_pollfd fds[2] = {
		{.fd = client.transport.tcp.sock, .events = ZSOCK_POLLIN},
		{.fd = evfd, .events = ZSOCK_POLLIN},
	};
	int ret = zsock_poll(fds, (evfd >= 0) ? 2 : 1, timeout_ms);

	if (ret < 0) {
		return -errno;
	}
	if (evfd >= 0 && (fds[1].revents & ZSOCK_POLLIN)) {
		zvfs_eventfd_t v;

		(void)zvfs_eventfd_read(evfd, &v);
	}
	if (fds[0].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
		return -ECONNRESET;
	}
	if (fds[0].revents & ZSOCK_POLLIN) {
		ret = mqtt_input(&client);
		if (ret < 0) {
			return ret;
		}
	}
	return session_lost ? -ECONNRESET : 0;
}

static int connect_broker(uint8_t bdseq)
{
	int64_t deadline;
	int ret;

	ret = resolve_broker();
	if (ret < 0) {
		return ret;
	}
	prepare_client(bdseq);
	connack_seen = false;
	session_lost = false;

	LOG_INF("connecting to %s:%u as '%s' (bdSeq %u)", cfg->mqtt.host, cfg->mqtt.port,
		cfg->mqtt.client_id, bdseq);
	ret = mqtt_connect(&client);
	if (ret < 0) {
		LOG_WRN("connect failed (%d)", ret);
		return ret;
	}

	deadline = k_uptime_get() + CONNACK_TIMEOUT_MS;
	while (!connack_seen && k_uptime_get() < deadline) {
		ret = poll_socket(100);
		if (ret < 0) {
			break;
		}
	}
	if (!connack_seen || connack_result != 0) {
		LOG_WRN("no CONNACK or connection refused (%d)", connack_seen ? connack_result : ret);
		(void)mqtt_abort(&client);
		return -ECONNREFUSED;
	}

	{
		const struct zsock_timeval tv = {.tv_sec = SEND_TIMEOUT_S};

		/* Bound the time a slow broker can block a publish. */
		(void)zsock_setsockopt(client.transport.tcp.sock, ZSOCK_SOL_SOCKET,
				       ZSOCK_SO_SNDTIMEO, &tv, sizeof(tv));
	}
	return 0;
}

static int subscribe_commands(void)
{
	struct mqtt_topic topics[] = {
		{.topic = {.utf8 = (const uint8_t *)ncmd_topic, .size = strlen(ncmd_topic)},
		 .qos = MQTT_QOS_1_AT_LEAST_ONCE},
		{.topic = {.utf8 = (const uint8_t *)dcmd_topic, .size = strlen(dcmd_topic)},
		 .qos = MQTT_QOS_1_AT_LEAST_ONCE},
	};
	const struct mqtt_subscription_list list = {
		.list = topics,
		.list_count = ARRAY_SIZE(topics),
		.message_id = next_msg_id(),
	};

	return mqtt_subscribe(&client, &list);
}

/* Orderly shutdown for NCMD Reboot: explicit NDEATH, then disconnect (SPB-2). */
static void shutdown_and_reboot(void)
{
	int64_t deadline = k_uptime_get() + 500;

	LOG_WRN("rebooting on request");
	if (death_len > 0 && link_publish(death_topic, death_buf, death_len, 1) == 0) {
		while (k_uptime_get() < deadline && poll_socket(50) == 0) {
		}
	}
	(void)mqtt_disconnect(&client, NULL);
	LOG_PANIC(); /* flush pending log messages before the reset */
	sys_reboot(SYS_REBOOT_COLD);
}

static void run_session(void)
{
	int ret;

	ret = subscribe_commands();
	if (ret < 0) {
		LOG_WRN("subscribe failed (%d)", ret);
		return;
	}
	model_status_set(GW_STATUS_MQTT_UP, true);

	while (true) {
		int timeout;

		feed_wdt();
		ret = spb_node_service();
		if (ret == SPB_NODE_REQ_REBOOT) {
			shutdown_and_reboot();
		}
		if (ret < 0) {
			break;
		}
		timeout = mqtt_keepalive_time_left(&client);
		ret = poll_socket(MIN(timeout, (evfd >= 0) ? POLL_MAX_MS : POLL_FALLBACK_MS));
		if (ret < 0) {
			LOG_WRN("session error (%d)", ret);
			break;
		}
		ret = mqtt_live(&client);
		if (ret < 0 && ret != -EAGAIN) {
			LOG_WRN("keepalive failed (%d)", ret);
			break;
		}
	}
	model_status_set(GW_STATUS_MQTT_UP, false);
}

static void backoff_sleep(uint32_t attempt)
{
	uint32_t delay = backoff_delay_ms(attempt, sys_rand32_get());

	LOG_INF("reconnecting in %u ms", delay);
	while (delay > 0) {
		uint32_t step = MIN(delay, 1000U);

		feed_wdt();
		k_msleep(step);
		delay -= step;
	}
}

static void link_main(void *p1, void *p2, void *p3)
{
	uint32_t attempt = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	wdt = task_wdt_add(CONFIG_GW_WDT_TIMEOUT_MS, NULL, NULL);

	/* Give SNTP a chance so the first NBIRTH carries a real UTC timestamp
	 * (TIME-1); without a time source the node still comes up (REL-6).
	 */
	if (cfg->ntp.server[0] != '\0' && netmgr_wait_ready(K_SECONDS(TIME_WAIT_S)) &&
	    !gw_time_wait_synced(K_SECONDS(TIME_WAIT_S))) {
		LOG_WRN("clock not synchronised: timestamps are relative to boot");
	}

	while (true) {
		uint8_t bdseq;

		feed_wdt();
		if (!netmgr_wait_ready(K_SECONDS(1))) {
			continue;
		}
		/* bdSeq advances on every attempt (SPB-3). */
		bdseq = bdseq_next();
		if (connect_broker(bdseq) == 0) {
			LOG_INF("MQTT connected");
			attempt = 0;
			spb_node_session_start(bdseq);
			run_session();
			spb_node_session_end();
			(void)mqtt_abort(&client);
			LOG_WRN("MQTT session closed");
		}
		backoff_sleep(attempt);
		if (attempt < 31) {
			attempt++;
		}
	}
}

int mqtt_link_start(const struct gw_config *config)
{
	const char *group = config->sparkplug.group_id;
	const char *node = config->sparkplug.edge_node_id;

	cfg = config;
	if (spb_topic(death_topic, sizeof(death_topic), group, "NDEATH", node, NULL) < 0 ||
	    spb_topic(ncmd_topic, sizeof(ncmd_topic), group, "NCMD", node, NULL) < 0 ||
	    spb_topic(dcmd_topic, sizeof(dcmd_topic), group, "DCMD", node, "+") < 0) {
		return -EINVAL;
	}
	if (config->sparkplug.primary_host_id[0] != '\0') {
		LOG_WRN("primary_host_id is set but SPB-13 (STATE) is not implemented: ignored");
	}

	evfd = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
	if (evfd < 0) {
		LOG_WRN("no eventfd (%d): falling back to periodic polling", errno);
	}
	spb_node_init(config, &node_ops, firmware, CONFIG_GW_HARDWARE_NAME);
	model_set_notify(wake);

	k_thread_create(&link_thread, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_main,
			NULL, NULL, NULL, K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&link_thread, "mqtt_link");
	return 0;
}
