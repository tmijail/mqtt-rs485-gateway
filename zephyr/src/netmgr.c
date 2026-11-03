/*
 * Network bring-up (NET-1..NET-3) and SNTP (TIME-1).
 *
 * With the native IP stack (W5500 in MACRAW mode) the interface is
 * configured with DHCP or a static address; the connection manager tells
 * when it is usable (link up + IPv4 address). Link loss and recovery are
 * handled by the driver and the stack (carrier on/off, DHCP renew), this
 * module only tracks the state. With offloaded sockets (native_sim) the
 * host owns addressing and only the readiness tracking applies.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "netmgr.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_mgmt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "gw_time.h"
#include "model.h"

LOG_MODULE_REGISTER(netmgr, CONFIG_GW_LOG_LEVEL);

#define EVT_READY BIT(0)

#define L4_EVENTS (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define IF_EVENTS (NET_EVENT_IF_UP | NET_EVENT_IF_DOWN)

static K_EVENT_DEFINE(net_events);
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback if_cb;
static const struct gw_config *cfg;

K_THREAD_STACK_DEFINE(net_stack, 2560);
static struct k_thread net_thread;

static void l4_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("network ready");
		model_status_set(GW_STATUS_NET_UP, true);
		k_event_post(&net_events, EVT_READY);
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		LOG_WRN("network lost");
		model_status_set(GW_STATUS_NET_UP, false);
		k_event_clear(&net_events, EVT_READY);
	}
}

static void if_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	LOG_INF("interface %d: link %s", net_if_get_by_iface(iface),
		event == NET_EVENT_IF_UP ? "up" : "down");
}

bool netmgr_wait_ready(k_timeout_t timeout)
{
	return k_event_wait(&net_events, EVT_READY, false, timeout) != 0;
}

#if !defined(CONFIG_NET_SOCKETS_OFFLOAD)

static void to_in_addr(const uint8_t b[4], struct net_in_addr *a)
{
	memcpy(&a->s4_addr, b, 4);
}

/* Ethernet MAC from the eFuse base MAC (+3, the ESP-IDF convention for
 * the Ethernet port), so every unit has a stable, unique address.
 */
static void set_mac(struct net_if *iface)
{
	struct ethernet_req_params params = {0};
	uint8_t id[6];
	ssize_t n;
	int ret;

	if (net_if_l2(iface) != &NET_L2_GET_NAME(ETHERNET)) {
		return;
	}
	n = hwinfo_get_device_id(id, sizeof(id));
	if (n != sizeof(id)) {
		LOG_WRN("no device id: keeping the driver MAC address");
		return;
	}
	memcpy(params.mac_address.addr, id, sizeof(id));
	params.mac_address.addr[5] = (uint8_t)(id[5] + 3U);
	params.mac_address.addr[0] &= (uint8_t)~0x01U; /* unicast */

	(void)net_if_down(iface);
	ret = net_mgmt(NET_REQUEST_ETHERNET_SET_MAC_ADDRESS, iface, &params, sizeof(params));
	if (ret < 0) {
		LOG_WRN("cannot set MAC address (%d)", ret);
	}
	(void)net_if_up(iface);
}

static int configure_ipv4(struct net_if *iface)
{
	struct net_in_addr addr, mask, gw;
	char dns[16];
	const char *servers[] = {dns, NULL};
	int ret;

	if (cfg->network.dhcp) {
		LOG_INF("IPv4 via DHCP");
		net_dhcpv4_start(iface);
		return 0;
	}

	to_in_addr(cfg->network.ip, &addr);
	to_in_addr(cfg->network.netmask, &mask);
	to_in_addr(cfg->network.gateway, &gw);
	if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("cannot set the static address");
		return -EINVAL;
	}
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);
	LOG_INF("IPv4 static %u.%u.%u.%u", cfg->network.ip[0], cfg->network.ip[1],
		cfg->network.ip[2], cfg->network.ip[3]);

	if (cfg->network.has_dns) {
		snprintf(dns, sizeof(dns), "%u.%u.%u.%u", cfg->network.dns[0], cfg->network.dns[1],
			 cfg->network.dns[2], cfg->network.dns[3]);
		ret = dns_resolve_reconfigure(dns_resolve_get_default(), servers, NULL,
					      DNS_SOURCE_MANUAL);
		if (ret < 0) {
			LOG_ERR("cannot set DNS server %s (%d)", dns, ret);
			return ret;
		}
	}
	return 0;
}

#endif /* !CONFIG_NET_SOCKETS_OFFLOAD */

static void net_main(void *p1, void *p2, void *p3)
{
	int wdt = task_wdt_add(CONFIG_GW_WDT_TIMEOUT_MS, NULL, NULL);
	int64_t next_sync = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		if (wdt >= 0) {
			(void)task_wdt_feed(wdt);
		}
		if (!netmgr_wait_ready(K_SECONDS(1))) {
			next_sync = 0; /* resync as soon as the network returns */
			continue;
		}
		if (cfg->ntp.server[0] == '\0' || k_uptime_get() < next_sync) {
			k_sleep(K_SECONDS(1));
			continue;
		}
		/* Retry quickly until the first success, then every resync period. */
		next_sync = k_uptime_get() +
			    ((gw_time_sntp_sync(cfg->ntp.server) == 0)
				     ? (int64_t)CONFIG_GW_SNTP_RESYNC_S * MSEC_PER_SEC
				     : 10 * MSEC_PER_SEC);
	}
}

int netmgr_start(const struct gw_config *config)
{
	struct net_if *iface = net_if_get_default();

	cfg = config;
	if (iface == NULL) {
		LOG_ERR("no network interface");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&l4_cb, l4_handler, L4_EVENTS);
	net_mgmt_add_event_callback(&l4_cb);
	net_mgmt_init_event_callback(&if_cb, if_handler, IF_EVENTS);
	net_mgmt_add_event_callback(&if_cb);

#if !defined(CONFIG_NET_SOCKETS_OFFLOAD)
	/* With offloaded sockets the host owns the interface and its addresses. */
	set_mac(iface);
	if (configure_ipv4(iface) < 0) {
		return -EINVAL;
	}
#endif
	/* Replay the current state in case the interface is already usable. */
	conn_mgr_mon_resend_status();

	k_thread_create(&net_thread, net_stack, K_THREAD_STACK_SIZEOF(net_stack), net_main, NULL,
			NULL, NULL, K_PRIO_PREEMPT(8), 0, K_NO_WAIT);
	k_thread_name_set(&net_thread, "netmgr");
	return 0;
}
