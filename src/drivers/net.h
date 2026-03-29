#ifndef NET_H
#define NET_H

/* IronKernel — net.h
   Common network types, byte-order helpers, and Internet checksum.
   All multi-byte protocol fields are in network (big-endian) order. */

#include "../kernel/types.h"

/* ── Byte-order helpers ──────────────────────────────────────────── */
static inline uint16_t htons(uint16_t v)
    { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v)
    { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0x000000FFu)
         | ((v >>  8) & 0x0000FF00u)
         | ((v <<  8) & 0x00FF0000u)
         | ((v << 24) & 0xFF000000u);
}
#define ntohl htonl

/* ── EtherType values ────────────────────────────────────────────── */
#define ETHERTYPE_ARP   0x0806u
#define ETHERTYPE_IP    0x0800u

/* ── IP protocol numbers ─────────────────────────────────────────── */
#define IP_PROTO_ICMP   1u
#define IP_PROTO_TCP    6u
#define IP_PROTO_UDP    17u

/* ── Ethernet header (14 bytes) ──────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;          /* big-endian EtherType */
} eth_hdr_t;

/* ── ARP packet for Ethernet/IPv4 (28 bytes) ─────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t hw_type;       /* 0x0001 = Ethernet */
    uint16_t proto_type;    /* 0x0800 = IPv4     */
    uint8_t  hw_len;        /* 6  */
    uint8_t  proto_len;     /* 4  */
    uint16_t op;            /* 1 = request, 2 = reply */
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} arp_pkt_t;

/* ── IPv4 header (20 bytes, no options) ─────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;       /* 0x45 = version 4, 5 dwords */
    uint8_t  tos;
    uint16_t total_len;     /* header + payload, big-endian */
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} ip_hdr_t;

/* ── ICMP header (8 bytes) ───────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  type;          /* 8 = echo request, 0 = echo reply */
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

/* ── TCP header (20 bytes, no options) ──────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack_seq;
    uint8_t  data_off;   /* bits 7-4: header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

/* TCP flag bits */
#define TCP_FIN  0x01u
#define TCP_SYN  0x02u
#define TCP_RST  0x04u
#define TCP_PSH  0x08u
#define TCP_ACK  0x10u

/* ── UDP header (8 bytes) ────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;        /* header + data */
    uint16_t checksum;      /* 0 = disabled */
} udp_hdr_t;

/* ── Internet checksum (RFC 1071) ────────────────────────────────── */
/* Reads data as big-endian 16-bit words; result stored directly
   in the header field (no byte-swap needed). */
static inline uint16_t net_checksum(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
        p += 2; len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

#endif
