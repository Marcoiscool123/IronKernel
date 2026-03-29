/* IronKernel — ip.c
   IPv4 / ICMP / UDP implementation.
   Guest: 10.0.2.15   Gateway: 10.0.2.2   (QEMU user-mode defaults) */

#include "ip.h"
#include "net.h"
#include "arp.h"
#include "e1000.h"
#include "tcp.h"
#include "../drivers/pit.h"

/* ── Network configuration (updated by DHCP, defaults to SLIRP) ─── */
uint8_t g_net_ip[4]  = { 10, 0, 2, 15 };
uint8_t g_net_gw[4]  = { 10, 0, 2,  2 };
uint8_t g_net_dns[4] = { 10, 0, 2,  3 };
static uint16_t g_ip_id = 1;

/* ── UDP receive buffer (one listener at a time) ─────────────────── */
#define UDP_RX_SIZE 512
static uint8_t  g_udp_rx_buf[UDP_RX_SIZE];
static uint16_t g_udp_rx_len  = 0;
static uint16_t g_udp_rx_port = 0;   /* 0 = not listening */

void udp_open(uint16_t local_port)  { g_udp_rx_port = local_port; g_udp_rx_len = 0; }
void udp_close(void)                { g_udp_rx_port = 0; g_udp_rx_len = 0; }

int udp_recv(void *buf, uint16_t maxlen)
{
    if (!g_udp_rx_len) return 0;
    uint16_t copy = (g_udp_rx_len < maxlen) ? g_udp_rx_len : maxlen;
    uint8_t *dst  = (uint8_t *)buf;
    for (uint16_t i = 0; i < copy; i++) dst[i] = g_udp_rx_buf[i];
    g_udp_rx_len = 0;
    return (int)copy;
}

/* ── ICMP reply state (set by icmp_recv, read by icmp_ping) ─────── */
static volatile int      g_ping_got   = 0;
static volatile uint32_t g_ping_ticks = 0;
static volatile uint16_t g_ping_id    = 0;
static volatile uint16_t g_ping_seq   = 0;

/* ── ip_send_raw ─────────────────────────────────────────────────── */
static int ip_send_raw(const uint8_t dst_mac[6], const uint8_t dst_ip[4],
                       uint8_t proto,
                       const void *payload, uint16_t plen)
{
    uint16_t frame_len = 14u + 20u + plen;
    if (frame_len > 1514u) return -1;

    /* Stack frame: 14 eth + 20 ip + up to 1480 bytes payload */
    uint8_t frame[1514];

    /* Ethernet header */
    eth_hdr_t *eth = (eth_hdr_t *)frame;
    uint8_t my_mac[6];
    e1000_mac(my_mac);
    for (int i = 0; i < 6; i++) eth->dst[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eth->src[i] = my_mac[i];
    eth->type = htons(ETHERTYPE_IP);

    /* IP header */
    ip_hdr_t *ip = (ip_hdr_t *)(frame + 14);
    ip->ver_ihl    = 0x45;
    ip->tos        = 0;
    ip->total_len  = htons(20u + plen);
    ip->id         = htons(g_ip_id++);
    ip->flags_frag = htons(0x4000);  /* Don't Fragment */
    ip->ttl        = 64;
    ip->protocol   = proto;
    ip->checksum   = 0;
    for (int i = 0; i < 4; i++) ip->src_ip[i] = g_net_ip[i];
    for (int i = 0; i < 4; i++) ip->dst_ip[i] = dst_ip[i];
    ip->checksum   = htons(net_checksum(ip, 20));

    /* Payload */
    const uint8_t *src = (const uint8_t *)payload;
    uint8_t       *dst = frame + 14 + 20;
    for (uint16_t i = 0; i < plen; i++) dst[i] = src[i];

    return e1000_send(frame, frame_len);
}

/* ── ip_send ─────────────────────────────────────────────────────── */
int ip_send(const uint8_t dst_ip[4], uint8_t proto,
            const void *payload, uint16_t payload_len)
{
    /* Routing: only ARP directly for addresses on our subnet (10.0.2.x).
       Everything else goes via the gateway at 10.0.2.2.
       The IP header still carries the real dst_ip; SLIRP forwards it. */
    int on_subnet = (dst_ip[0] == g_net_ip[0] &&
                     dst_ip[1] == g_net_ip[1] &&
                     dst_ip[2] == g_net_ip[2]);
    const uint8_t *arp_target = on_subnet ? dst_ip : g_net_gw;

    uint8_t dst_mac[6];
    if (arp_resolve(arp_target, dst_mac) != 0) return -1;
    return ip_send_raw(dst_mac, dst_ip, proto, payload, payload_len);
}

/* ── ICMP receive ────────────────────────────────────────────────── */
static void icmp_recv(const uint8_t *frame, uint16_t len)
{
    if (len < 14u + 20u + 8u) return;
    const ip_hdr_t   *ip   = (const ip_hdr_t   *)(frame + 14);
    const icmp_hdr_t *icmp = (const icmp_hdr_t *)(frame + 14 + 20);

    /* Only care about echo replies matching our outstanding ping */
    if (ip->protocol != IP_PROTO_ICMP) return;
    if (icmp->type != 0)               return;   /* 0 = echo reply */
    if (ntohs(icmp->id)  != g_ping_id) return;
    if (ntohs(icmp->seq) != g_ping_seq) return;

    g_ping_ticks = pit_get_ticks();
    g_ping_got   = 1;
}

/* ── icmp_ping ───────────────────────────────────────────────────── */
int icmp_ping(const uint8_t dst_ip[4], uint16_t seq)
{
    /* Build ICMP echo request: 8-byte header + 32-byte payload */
    uint8_t pkt[40];
    icmp_hdr_t *icmp = (icmp_hdr_t *)pkt;
    icmp->type     = 8;         /* echo request */
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(0x494B);   /* 'IK' */
    icmp->seq      = htons(seq);
    for (int i = 8; i < 40; i++) pkt[i] = (uint8_t)(i - 8);
    icmp->checksum = htons(net_checksum(pkt, 40));

    /* ARP-resolve destination — use gateway for off-subnet addresses */
    int on_sub = (dst_ip[0] == g_net_ip[0] &&
                  dst_ip[1] == g_net_ip[1] &&
                  dst_ip[2] == g_net_ip[2]);
    uint8_t dst_mac[6];
    if (arp_resolve(on_sub ? dst_ip : g_net_gw, dst_mac) != 0) return -1;

    /* Arm reply state before sending (avoid race with very fast replies) */
    g_ping_id  = 0x494B;
    g_ping_seq = seq;
    g_ping_got = 0;

    uint32_t t0 = pit_get_ticks();
    if (ip_send_raw(dst_mac, dst_ip, IP_PROTO_ICMP, pkt, 40) != 0) return -1;

    /* Poll for reply — dual timeout: iteration cap + tick-based (3 s) */
    for (long iter = 0; iter < 24000000L; iter++) {
        uint8_t  buf[2048];
        uint16_t rlen;
        if (e1000_recv(buf, &rlen) == 0) net_recv(buf, rlen);
        if (g_ping_got)
            return (int)((g_ping_ticks - t0) * 10u);  /* RTT in ms */
        if ((pit_get_ticks() - t0) >= 300u) break;     /* 3 s tick timeout */
    }
    return -1;
}

/* ── udp_send ────────────────────────────────────────────────────── */
int udp_send(const uint8_t dst_ip[4],
             uint16_t src_port, uint16_t dst_port,
             const void *data, uint16_t len)
{
    uint16_t udp_len = 8u + len;
    if (udp_len > 1480u) return -1;

    uint8_t pkt[1480];
    udp_hdr_t *udp = (udp_hdr_t *)pkt;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons(udp_len);
    udp->checksum = 0;  /* checksum optional in IPv4 */

    const uint8_t *src = (const uint8_t *)data;
    for (uint16_t i = 0; i < len; i++) pkt[8 + i] = src[i];

    return ip_send(dst_ip, IP_PROTO_UDP, pkt, udp_len);
}

/* ── net_recv — dispatch received Ethernet frame ─────────────────── */
void net_recv(const uint8_t *frame, uint16_t len)
{
    if (len < 14u) return;
    uint16_t type = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);

    if (type == ETHERTYPE_ARP) {
        arp_recv(frame, len);
    } else if (type == ETHERTYPE_IP) {
        if (len < 14u + 20u) return;
        const ip_hdr_t *ip = (const ip_hdr_t *)(frame + 14);
        if (ip->protocol == IP_PROTO_ICMP) icmp_recv(frame, len);
        if (ip->protocol == IP_PROTO_TCP)  tcp_recv_frame(frame, len);
        if (ip->protocol == IP_PROTO_UDP && g_udp_rx_port) {
            if (len >= 14u + 20u + 8u) {
                const udp_hdr_t *udp = (const udp_hdr_t *)(frame + 14 + 20);
                if (ntohs(udp->dst_port) == g_udp_rx_port) {
                    uint16_t data_len = (uint16_t)(ntohs(udp->length) - 8u);
                    if (data_len > UDP_RX_SIZE) data_len = UDP_RX_SIZE;
                    const uint8_t *data = frame + 14u + 20u + 8u;
                    for (uint16_t i = 0; i < data_len; i++) g_udp_rx_buf[i] = data[i];
                    g_udp_rx_len = data_len;
                }
            }
        }
    }
}
