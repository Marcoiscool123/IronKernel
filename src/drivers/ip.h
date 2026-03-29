#ifndef IP_H
#define IP_H

#include "../kernel/types.h"

/* ── Global network configuration (set by DHCP, defaults to SLIRP) ─ */
extern uint8_t g_net_ip[4];    /* our IPv4 address          */
extern uint8_t g_net_gw[4];    /* default gateway           */
extern uint8_t g_net_dns[4];   /* DNS server                */

/* ── IPv4 / ICMP / UDP send ──────────────────────────────────────── */

/* Send a raw IPv4 packet with the given payload.
   ARP-resolves dst_ip automatically.
   Returns 0 on success, -1 on ARP failure or TX error. */
int ip_send(const uint8_t dst_ip[4], uint8_t proto,
            const void *payload, uint16_t payload_len);

/* Send an ICMP echo request with the given sequence number.
   Polls for the echo reply; returns RTT in milliseconds,
   or -1 on timeout (3 s). */
int icmp_ping(const uint8_t dst_ip[4], uint16_t seq);

/* Send a UDP datagram.
   Returns 0 on success, -1 on ARP failure or TX error. */
int udp_send(const uint8_t dst_ip[4],
             uint16_t src_port, uint16_t dst_port,
             const void *data, uint16_t len);

/* UDP receive — one listener at a time.
   udp_open()  — start buffering packets arriving on local_port.
   udp_recv()  — copy buffered payload into buf; returns byte count or 0.
   udp_close() — stop listening and clear the buffer.
   The buffer holds the most recent packet only (512 bytes max). */
void udp_open(uint16_t local_port);
int  udp_recv(void *buf, uint16_t maxlen);
void udp_close(void);

/* Dispatch a received Ethernet frame to the appropriate protocol handler.
   Call this from any polling loop that reads raw frames from e1000_recv(). */
void net_recv(const uint8_t *frame, uint16_t len);

#endif
