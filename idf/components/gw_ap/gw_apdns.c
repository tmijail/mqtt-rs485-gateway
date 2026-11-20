/*
 * Issue #274 (NET-5): the DNS server of the captive portal of the access point.
 *
 * Where it runs: in the tcpip task of lwIP (core 0), as the receive callback of a raw UDP PCB on
 * port 53, and not in a task of its own. A query needs a few hundred bytes of work and no wait:
 * one parse, one answer (gw_captive_dns_answer), one send. A task would cost a stack in internal
 * RAM for each boot, also while the access point is off (#80). The PCB is bound to the interface
 * of the access point (udp_bind_netif), so lwIP never gives it a datagram of another interface
 * (the Ethernet or the Wi-Fi station of the plant network). The callback also applies the rule of
 * the claim window to the source (gw_claim_from_ap: a host of the subnet of that interface).
 *
 * Its memory (REL-2): the state is static (the PCB pointer and two counters). The query is read
 * in the receive buffer of lwIP, and the answer is written into one send buffer of lwIP
 * (pbuf_alloc, as each socket does for each datagram; lwIP takes it from the PSRAM first:
 * CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP). The server has no buffer of its own. lwIP allocates the
 * PCB when the server starts and frees it when it stops. A query in more than one buffer (an IP
 * fragment) gets no answer: a DNS query of a phone is one small datagram.
 */
#include "gw_apdns.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_netif.h"
#include "gw_captive.h"
#include "gw_claim.h"
#include "lwip/ip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

static struct udp_pcb *s_pcb;   /* changed only in the tcpip task (esp_netif_tcpip_exec) */
static atomic_bool s_running;
static atomic_uint s_answered, s_dropped;

/* The receive callback: in the tcpip task. */
static void on_query(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
                     u16_t port)
{
    (void)arg;
    struct netif *nif = ip_current_input_netif();
    bool sent = false;
    if (nif != NULL && addr != NULL && IP_IS_V4(addr) && p->len == p->tot_len &&
        p->len <= GW_CAPTIVE_DNS_QUERY_MAX) {
        gw_claim_if_t ap;
        uint8_t peer[4];
        memcpy(ap.ip, &netif_ip4_addr(nif)->addr, 4);
        memcpy(ap.mask, &netif_ip4_netmask(nif)->addr, 4);
        memcpy(peer, &ip_2_ip4(addr)->addr, 4);
        if (gw_claim_from_ap(ap.ip, peer, &ap, NULL, 0)) {
            struct pbuf *r = pbuf_alloc(PBUF_TRANSPORT, (u16_t)(p->len + GW_CAPTIVE_DNS_A_RR),
                                        PBUF_RAM);
            if (r != NULL) {
                size_t n = gw_captive_dns_answer(p->payload, p->len, ap.ip, r->payload, r->len);
                if (n != 0) {
                    pbuf_realloc(r, (u16_t)n);
                    sent = udp_sendto_if(pcb, r, addr, port, nif) == ERR_OK;
                }
                pbuf_free(r);
            }
        }
    }
    atomic_fetch_add(sent ? &s_answered : &s_dropped, 1u);
    pbuf_free(p);
}

static esp_err_t do_stop(void *ctx)
{
    (void)ctx;
    if (s_pcb != NULL) {
        udp_remove(s_pcb);
        s_pcb = NULL;
    }
    return ESP_OK;
}

static esp_err_t do_start(void *ctx)
{
    do_stop(NULL);
    struct netif *nif = netif_get_by_index((u8_t)*(const int *)ctx);
    if (nif == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        return ESP_ERR_NO_MEM;
    }
    udp_bind_netif(pcb, nif);
    if (udp_bind(pcb, IP4_ADDR_ANY, GW_CAPTIVE_DNS_PORT) != ERR_OK) {
        udp_remove(pcb);
        return ESP_ERR_INVALID_STATE;   /* port 53 in use */
    }
    udp_recv(pcb, on_query, NULL);
    s_pcb = pcb;
    return ESP_OK;
}

esp_err_t gw_apdns_start(int netif_index)
{
    if (netif_index <= 0 || netif_index > 255) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t e = esp_netif_tcpip_exec(do_start, &netif_index);
    atomic_store(&s_running, e == ESP_OK);
    return e;
}

void gw_apdns_stop(void)
{
    if (atomic_exchange(&s_running, false)) {
        esp_netif_tcpip_exec(do_stop, NULL);
    }
}

bool gw_apdns_running(void)
{
    return atomic_load(&s_running);
}

void gw_apdns_counts(uint32_t *answered, uint32_t *dropped)
{
    *answered = atomic_load(&s_answered);
    *dropped = atomic_load(&s_dropped);
}
