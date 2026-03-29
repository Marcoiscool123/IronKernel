/* IronKernel — dhcp.c
   DHCP client: DORA sequence (Discover → Offer → Request → ACK).
   Sends raw Ethernet/IP/UDP frames (src=0.0.0.0) before we have an IP.
   On success, updates g_net_ip, g_net_gw, g_net_dns.              */

#include "dhcp.h"
#include "ip.h"
#include "arp.h"
#include "net.h"
#include "e1000.h"
#include "../drivers/pit.h"

#define DHCP_SERVER_PORT  67u
#define DHCP_CLIENT_PORT  68u
#define DHCP_TIMEOUT      200u   /* 2 s at 100 Hz */
#define DHCP_MAGIC        0x63825363UL

/* ── Build a raw Ethernet broadcast frame for DHCP ───────────────── */
static int dhcp_send_raw(const uint8_t *udp_data, uint16_t udp_dlen)
{
    uint8_t  frame[600];
    uint16_t n = 0;

    /* Ethernet: broadcast dst, our MAC src */
    uint8_t mac[6];
    e1000_mac(mac);
    for (int i = 0; i < 6; i++) frame[n++] = 0xFF;
    for (int i = 0; i < 6; i++) frame[n++] = mac[i];
    frame[n++] = 0x08; frame[n++] = 0x00;   /* EtherType = IPv4 */

    /* IPv4 header: src=0.0.0.0  dst=255.255.255.255 */
    uint8_t *iph = frame + n;
    uint16_t ip_total = 20u + 8u + udp_dlen;
    iph[0]  = 0x45;                           /* ver=4 ihl=5 */
    iph[1]  = 0x00;                           /* DSCP/ECN    */
    iph[2]  = (uint8_t)(ip_total >> 8);
    iph[3]  = (uint8_t)(ip_total & 0xFF);
    iph[4]  = 0x00; iph[5] = 0x01;           /* ID          */
    iph[6]  = 0x00; iph[7] = 0x00;           /* flags/frag  */
    iph[8]  = 64;                             /* TTL         */
    iph[9]  = 17;                             /* protocol UDP */
    iph[10] = 0x00; iph[11] = 0x00;          /* checksum    */
    iph[12] = 0; iph[13] = 0; iph[14] = 0; iph[15] = 0;           /* src 0.0.0.0 */
    iph[16] = 255; iph[17] = 255; iph[18] = 255; iph[19] = 255;   /* dst bcast   */
    n += 20;
    /* IP checksum */
    uint16_t ck = net_checksum(iph, 20);
    iph[10] = (uint8_t)(ck >> 8); iph[11] = (uint8_t)(ck & 0xFF);

    /* UDP header: src=68  dst=67 */
    uint16_t udp_total = 8u + udp_dlen;
    frame[n++] = 0x00; frame[n++] = DHCP_CLIENT_PORT;
    frame[n++] = 0x00; frame[n++] = DHCP_SERVER_PORT;
    frame[n++] = (uint8_t)(udp_total >> 8);
    frame[n++] = (uint8_t)(udp_total & 0xFF);
    frame[n++] = 0x00; frame[n++] = 0x00;   /* checksum optional */

    /* UDP payload */
    for (uint16_t i = 0; i < udp_dlen; i++) frame[n++] = udp_data[i];

    return e1000_send(frame, n);
}

/* ── Build the 236-byte BOOTP header + magic cookie + options ──── */
static uint16_t dhcp_build_pkt(uint8_t *pkt, uint8_t msg_type,
                                uint32_t xid,
                                const uint8_t offered_ip[4],
                                const uint8_t server_ip[4])
{
    uint16_t n = 0;
    /* Zero the packet first (covers sname + file padding) */
    for (int i = 0; i < 300; i++) pkt[i] = 0;

    pkt[n++] = 1;    /* op:    BOOTREQUEST */
    pkt[n++] = 1;    /* htype: Ethernet    */
    pkt[n++] = 6;    /* hlen:  6           */
    pkt[n++] = 0;    /* hops               */

    /* xid */
    pkt[n++] = (uint8_t)(xid >> 24);
    pkt[n++] = (uint8_t)(xid >> 16);
    pkt[n++] = (uint8_t)(xid >>  8);
    pkt[n++] = (uint8_t)(xid & 0xFF);

    pkt[n++] = 0x00; pkt[n++] = 0x00;   /* secs  */
    pkt[n++] = 0x80; pkt[n++] = 0x00;   /* flags: BROADCAST */

    n += 4;   /* ciaddr = 0.0.0.0 */
    n += 4;   /* yiaddr = 0.0.0.0 */
    n += 4;   /* siaddr = 0.0.0.0 */
    n += 4;   /* giaddr = 0.0.0.0 */

    /* chaddr: our MAC in first 6 bytes, rest zero */
    uint8_t mac[6]; e1000_mac(mac);
    for (int i = 0; i < 6; i++) pkt[n++] = mac[i];
    n += 10;    /* pad chaddr to 16 bytes */

    n += 64;    /* sname */
    n += 128;   /* file  */

    /* Magic cookie */
    pkt[n++] = 0x63; pkt[n++] = 0x82; pkt[n++] = 0x53; pkt[n++] = 0x63;

    /* Option 53: DHCP Message Type */
    pkt[n++] = 53; pkt[n++] = 1; pkt[n++] = msg_type;

    if (msg_type == 3) {   /* DHCP Request */
        /* Option 50: Requested IP */
        pkt[n++] = 50; pkt[n++] = 4;
        for (int i = 0; i < 4; i++) pkt[n++] = offered_ip[i];
        /* Option 54: Server Identifier */
        pkt[n++] = 54; pkt[n++] = 4;
        for (int i = 0; i < 4; i++) pkt[n++] = server_ip[i];
    }

    /* Option 55: Parameter Request List (subnet, router, DNS) */
    pkt[n++] = 55; pkt[n++] = 3;
    pkt[n++] = 1;    /* subnet mask */
    pkt[n++] = 3;    /* router      */
    pkt[n++] = 6;    /* DNS         */

    /* Option 255: End */
    pkt[n++] = 255;

    return n;
}

/* ── Parse option value from DHCP options blob ──────────────────── */
static int dhcp_opt(const uint8_t *opts, uint16_t opts_len,
                    uint8_t code, uint8_t *out, uint8_t expected_len)
{
    uint16_t i = 0;
    while (i < opts_len) {
        uint8_t c = opts[i++];
        if (c == 255) break;
        if (c == 0)   continue;   /* pad */
        if (i >= opts_len) break;
        uint8_t l = opts[i++];
        if (c == code && l == expected_len && i + l <= opts_len) {
            for (uint8_t j = 0; j < l; j++) out[j] = opts[i + j];
            return 0;
        }
        i += l;
    }
    return -1;
}

/* ── Poll until a UDP packet arrives on port 68 ──────────────────── */
static int dhcp_wait(uint8_t *resp, uint16_t maxlen, uint32_t timeout)
{
    uint32_t t0 = pit_get_ticks();
    for (long iter = 0; iter < 20000000L; iter++) {
        uint8_t  buf[2048];
        uint16_t flen;
        if (e1000_recv(buf, &flen) == 0) net_recv(buf, flen);
        int r = udp_recv(resp, maxlen);
        if (r > 0) return r;
        if ((pit_get_ticks() - t0) >= timeout) return -1;
    }
    return -1;
}

/* ── dhcp_discover — full DORA ───────────────────────────────────── */
int dhcp_discover(void)
{
    uint8_t pkt[300];
    uint8_t resp[600];

    /* Use PIT ticks as transaction ID */
    uint32_t xid = pit_get_ticks() | 0x494B0000UL;

    /* ── Discover ── */
    uint16_t plen = dhcp_build_pkt(pkt, 1, xid, 0, 0);
    udp_open(DHCP_CLIENT_PORT);
    if (dhcp_send_raw(pkt, plen) != 0) { udp_close(); return -1; }

    /* ── Wait for Offer ── */
    int rlen = dhcp_wait(resp, sizeof(resp), DHCP_TIMEOUT);
    if (rlen < 240) { udp_close(); return -1; }

    /* Validate: op=2 (reply), xid match, magic cookie */
    if (resp[0] != 2) { udp_close(); return -1; }
    uint32_t rxid = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16)
                  | ((uint32_t)resp[6] <<  8) |  (uint32_t)resp[7];
    if (rxid != xid)  { udp_close(); return -1; }
    if (resp[236] != 0x63 || resp[237] != 0x82 ||
        resp[238] != 0x53 || resp[239] != 0x63) { udp_close(); return -1; }

    /* Check DHCP message type == 2 (Offer) */
    uint8_t mtype = 0;
    dhcp_opt(resp + 240, (uint16_t)(rlen - 240), 53, &mtype, 1);
    if (mtype != 2) { udp_close(); return -1; }

    /* Extract offered IP (yiaddr at offset 16) and server IP */
    uint8_t offered[4], server[4];
    for (int i = 0; i < 4; i++) offered[i] = resp[16 + i];

    /* Server ID from option 54, fall back to siaddr (offset 20) */
    if (dhcp_opt(resp + 240, (uint16_t)(rlen - 240), 54, server, 4) != 0)
        for (int i = 0; i < 4; i++) server[i] = resp[20 + i];

    /* ── Request ── */
    plen = dhcp_build_pkt(pkt, 3, xid, offered, server);
    if (dhcp_send_raw(pkt, plen) != 0) { udp_close(); return -1; }

    /* ── Wait for ACK ── */
    rlen = dhcp_wait(resp, sizeof(resp), DHCP_TIMEOUT);
    udp_close();
    if (rlen < 240) return -1;

    rxid = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16)
         | ((uint32_t)resp[6] <<  8) |  (uint32_t)resp[7];
    if (rxid != xid) return -1;

    dhcp_opt(resp + 240, (uint16_t)(rlen - 240), 53, &mtype, 1);
    if (mtype != 5) return -1;   /* must be ACK */

    /* ── Commit configuration ── */
    for (int i = 0; i < 4; i++) g_net_ip[i]  = resp[16 + i];

    uint8_t gw[4] = {0}, dns[4] = {0};
    dhcp_opt(resp + 240, (uint16_t)(rlen - 240), 3, gw,  4);
    dhcp_opt(resp + 240, (uint16_t)(rlen - 240), 6, dns, 4);
    if (gw[0]  || gw[1]  || gw[2]  || gw[3])
        for (int i = 0; i < 4; i++) g_net_gw[i]  = gw[i];
    if (dns[0] || dns[1] || dns[2] || dns[3])
        for (int i = 0; i < 4; i++) g_net_dns[i] = dns[i];

    /* Pre-warm the ARP cache for the gateway so the first TCP connection
       (wget, ping, etc.) doesn't have to wait for ARP on a cold cache.
       Discard the result — we just want the cache entry. */
    { uint8_t gw_mac[6]; arp_resolve(g_net_gw, gw_mac); }

    return 0;
}
