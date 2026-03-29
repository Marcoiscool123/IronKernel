/* IronKernel — arp.c
   ARP layer: cache, request, reply.
   Guest IP is fixed at 10.0.2.15 (QEMU user-mode networking default). */

#include "arp.h"
#include "net.h"
#include "e1000.h"
#include "../drivers/pit.h"

/* Guest IP comes from the global network config in ip.c */
#include "ip.h"

/* ── ARP cache ───────────────────────────────────────────────────── */
#define ARP_CACHE_SIZE 8

static struct {
    uint8_t ip[4];
    uint8_t mac[6];
    int     valid;
} arp_cache[ARP_CACHE_SIZE];

static int arp_cache_lookup(const uint8_t ip[4], uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) continue;
        if (arp_cache[i].ip[0] != ip[0] || arp_cache[i].ip[1] != ip[1] ||
            arp_cache[i].ip[2] != ip[2] || arp_cache[i].ip[3] != ip[3]) continue;
        for (int j = 0; j < 6; j++) mac[j] = arp_cache[i].mac[j];
        return 0;
    }
    return -1;
}

static void arp_cache_insert(const uint8_t ip[4], const uint8_t mac[6])
{
    /* Find an existing entry for this IP, or the first free slot. */
    int slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid && slot < 0) { slot = i; continue; }
        if (!arp_cache[i].valid) continue;
        if (arp_cache[i].ip[0] == ip[0] && arp_cache[i].ip[1] == ip[1] &&
            arp_cache[i].ip[2] == ip[2] && arp_cache[i].ip[3] == ip[3]) {
            slot = i; break;
        }
    }
    if (slot < 0) slot = 0;  /* evict slot 0 if cache is full */
    for (int i = 0; i < 4; i++) arp_cache[slot].ip[i]  = ip[i];
    for (int i = 0; i < 6; i++) arp_cache[slot].mac[i] = mac[i];
    arp_cache[slot].valid = 1;
}

/* ── Send ARP request ────────────────────────────────────────────── */
static void arp_send_request(const uint8_t target_ip[4])
{
    uint8_t my_mac[6];
    e1000_mac(my_mac);

    uint8_t frame[42];  /* 14 ethernet + 28 ARP */

    /* Ethernet header: broadcast destination */
    for (int i = 0; i < 6; i++) frame[i]     = 0xFF;
    for (int i = 0; i < 6; i++) frame[6 + i] = my_mac[i];
    frame[12] = 0x08; frame[13] = 0x06;  /* EtherType ARP */

    /* ARP body */
    arp_pkt_t *arp = (arp_pkt_t *)(frame + 14);
    arp->hw_type    = htons(0x0001);
    arp->proto_type = htons(0x0800);
    arp->hw_len     = 6;
    arp->proto_len  = 4;
    arp->op         = htons(1);          /* request */
    for (int i = 0; i < 6; i++) arp->sender_mac[i] = my_mac[i];
    for (int i = 0; i < 4; i++) arp->sender_ip[i]  = g_net_ip[i];
    for (int i = 0; i < 6; i++) arp->target_mac[i] = 0;
    for (int i = 0; i < 4; i++) arp->target_ip[i]  = target_ip[i];

    e1000_send(frame, 42);
}

/* ── arp_recv ─────────────────────────────────────────────────────── */
void arp_recv(const uint8_t *frame, uint16_t len)
{
    if (len < 42) return;
    const arp_pkt_t *arp = (const arp_pkt_t *)(frame + 14);
    if (ntohs(arp->hw_type)    != 0x0001) return;
    if (ntohs(arp->proto_type) != 0x0800) return;

    /* Cache sender's MAC on every valid ARP packet (request or reply) */
    arp_cache_insert(arp->sender_ip, arp->sender_mac);

    /* If it's a request directed at our IP, send a unicast reply */
    if (ntohs(arp->op) == 1) {
        if (arp->target_ip[0] != g_net_ip[0] || arp->target_ip[1] != g_net_ip[1] ||
            arp->target_ip[2] != g_net_ip[2] || arp->target_ip[3] != g_net_ip[3])
            return;  /* not for us */

        uint8_t my_mac[6];
        e1000_mac(my_mac);

        uint8_t reply[42];
        eth_hdr_t *reth = (eth_hdr_t *)reply;
        for (int i = 0; i < 6; i++) reth->dst[i] = arp->sender_mac[i];
        for (int i = 0; i < 6; i++) reth->src[i] = my_mac[i];
        reth->type = htons(ETHERTYPE_ARP);

        arp_pkt_t *rarp = (arp_pkt_t *)(reply + 14);
        rarp->hw_type    = htons(0x0001);
        rarp->proto_type = htons(0x0800);
        rarp->hw_len     = 6;
        rarp->proto_len  = 4;
        rarp->op         = htons(2);    /* reply */
        for (int i = 0; i < 6; i++) rarp->sender_mac[i] = my_mac[i];
        for (int i = 0; i < 4; i++) rarp->sender_ip[i]  = g_net_ip[i];
        for (int i = 0; i < 6; i++) rarp->target_mac[i] = arp->sender_mac[i];
        for (int i = 0; i < 4; i++) rarp->target_ip[i]  = arp->sender_ip[i];

        e1000_send(reply, 42);
    }
}

/* ── arp_resolve ──────────────────────────────────────────────────── */
int arp_resolve(const uint8_t ip[4], uint8_t mac[6])
{
    if (arp_cache_lookup(ip, mac) == 0) return 0;

    /* 3 attempts × ~1 second each.
       Inner loop uses both an iteration cap (works even if PIT ticks stall)
       and a tick-based timeout (more accurate when PIT is running). */
    for (int attempt = 0; attempt < 3; attempt++) {
        arp_send_request(ip);

        uint32_t t0 = pit_get_ticks();
        for (long iter = 0; iter < 8000000L; iter++) {
            uint8_t  buf[2048];
            uint16_t rlen;
            if (e1000_recv(buf, &rlen) == 0 && rlen >= 14) {
                uint16_t type = (uint16_t)(((uint16_t)buf[12] << 8) | buf[13]);
                if (type == ETHERTYPE_ARP) arp_recv(buf, rlen);
                if (arp_cache_lookup(ip, mac) == 0) return 0;
            }
            /* Also break on tick-based timeout (~1 s) */
            if ((pit_get_ticks() - t0) >= 100u) break;
        }
        if (arp_cache_lookup(ip, mac) == 0) return 0;
    }
    return -1;
}
