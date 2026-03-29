/* IronKernel — dns.c
   DNS A-record resolver over UDP.
   Sends a single query to the SLIRP DNS forwarder at 10.0.2.3:53,
   polls for the response, and collects all A-record answers.
   Uses the kernel's UDP receive path (udp_open/udp_recv in ip.c). */

#include "dns.h"
#include "ip.h"
#include "e1000.h"
#include "../drivers/pit.h"

/* DNS server is g_net_dns from ip.h (updated by DHCP) */
#define DNS_SERVER g_net_dns

#define DNS_PORT     53u
#define DNS_SRC_PORT 5353u
#define DNS_TIMEOUT  300u       /* 3 s at 100 Hz PIT */
#define DNS_TXID     0x494Bu    /* 'IK' */

/* ── Encode hostname as DNS label wire format ─────────────────────
   "neverssl.com" → \x08neverssl\x03com\x00                         */
static uint16_t dns_encode_name(const char *host, uint8_t *out)
{
    uint16_t pos = 0;
    while (*host) {
        const char *dot = host;
        while (*dot && *dot != '.') dot++;
        uint8_t label_len = (uint8_t)(dot - host);
        out[pos++] = label_len;
        for (uint8_t i = 0; i < label_len; i++)
            out[pos++] = (uint8_t)host[i];
        host = dot;
        if (*host == '.') host++;
    }
    out[pos++] = 0;
    return pos;
}

/* ── Skip a DNS name (handles compression pointers).
   Returns offset of the byte immediately after the name field.      */
static uint16_t dns_skip_name(const uint8_t *buf, uint16_t off, uint16_t len)
{
    while (off < len) {
        uint8_t b = buf[off];
        if (b == 0)             return off + 1;   /* root label */
        if ((b & 0xC0) == 0xC0) return off + 2;   /* 2-byte pointer */
        off += 1u + b;
    }
    return off;
}

/* ── Expand a DNS name from wire format into a C string.
   Follows compression pointers (0xC0xx).
   Writes up to outmax-1 chars + NUL into out.
   Returns number of chars written (not counting NUL).               */
static uint16_t dns_expand_name(const uint8_t *buf, uint16_t off,
                                uint16_t len, char *out, uint16_t outmax)
{
    uint16_t written = 0;
    int      jumped  = 0;   /* have we followed a pointer yet? */
    int      hops    = 0;   /* guard against pointer loops     */

    while (off < len && hops < 16) {
        uint8_t b = buf[off];
        if (b == 0) break;                         /* root label */
        if ((b & 0xC0) == 0xC0) {                 /* compression pointer */
            off = (uint16_t)(((b & 0x3F) << 8) | buf[off + 1]);
            jumped = 1; hops++;
            continue;
        }
        /* Normal label */
        uint8_t llen = b;
        off++;
        if (written > 0 && written < (uint16_t)(outmax - 1))
            out[written++] = '.';
        for (uint8_t i = 0; i < llen && off < len; i++, off++) {
            if (written < (uint16_t)(outmax - 1))
                out[written++] = (char)buf[off];
        }
        if (jumped) continue;   /* already advanced by pointer logic */
    }
    if (outmax > 0) out[written] = '\0';
    (void)jumped;
    return written;
}

/* ── dns_reverse ─────────────────────────────────────────────────── */
int dns_reverse(const uint8_t ip[4], char *name_out, uint16_t maxlen)
{
    /* Build PTR query name: reverse octets + ".in-addr.arpa"
       e.g. 8.8.8.8 → "8.8.8.8.in-addr.arpa"                        */
    char qname[64];
    /* Build octet strings for each byte */
    uint8_t o[4][4];  /* up to 3 digits + NUL per octet */
    for (int i = 0; i < 4; i++) {
        uint8_t v = ip[3 - i];   /* reversed order */
        int pos = 0;
        if (v >= 100) { o[i][pos++] = (uint8_t)('0' + v / 100); }
        if (v >=  10) { o[i][pos++] = (uint8_t)('0' + (v / 10) % 10); }
        o[i][pos++] = (uint8_t)('0' + v % 10);
        o[i][pos]   = 0;
    }
    /* Assemble "d.c.b.a.in-addr.arpa" */
    int qi = 0;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; o[i][j]; j++) qname[qi++] = (char)o[i][j];
        qname[qi++] = '.';
    }
    const char *suffix = "in-addr.arpa";
    for (; *suffix; suffix++) qname[qi++] = *suffix;
    qname[qi] = '\0';

    /* Build DNS query packet */
    uint8_t  pkt[512];
    uint16_t n = 0;
    pkt[n++] = (uint8_t)(DNS_TXID >> 8);
    pkt[n++] = (uint8_t)(DNS_TXID & 0xFF);
    pkt[n++] = 0x01; pkt[n++] = 0x00;   /* RD=1 */
    pkt[n++] = 0x00; pkt[n++] = 0x01;   /* QDCOUNT=1 */
    pkt[n++] = 0x00; pkt[n++] = 0x00;
    pkt[n++] = 0x00; pkt[n++] = 0x00;
    pkt[n++] = 0x00; pkt[n++] = 0x00;
    n += dns_encode_name(qname, pkt + n);
    pkt[n++] = 0x00; pkt[n++] = 0x0C;   /* QTYPE  = PTR (12) */
    pkt[n++] = 0x00; pkt[n++] = 0x01;   /* QCLASS = IN       */

    /* Send and poll */
    udp_open(DNS_SRC_PORT);
    if (udp_send(DNS_SERVER, DNS_SRC_PORT, DNS_PORT, pkt, n) != 0) {
        udp_close(); return -1;
    }

    uint8_t  resp[512];
    int      rlen = 0;
    uint32_t t0   = pit_get_ticks();
    for (long iter = 0; iter < 30000000L; iter++) {
        uint8_t  buf[2048]; uint16_t flen;
        if (e1000_recv(buf, &flen) == 0) net_recv(buf, flen);
        rlen = udp_recv(resp, (uint16_t)sizeof(resp));
        if (rlen > 0) break;
        if ((pit_get_ticks() - t0) >= DNS_TIMEOUT) break;
    }
    udp_close();

    if (rlen < 12) return -1;
    if (resp[0] != (uint8_t)(DNS_TXID >> 8) ||
        resp[1] != (uint8_t)(DNS_TXID & 0xFF)) return -1;
    if (!(resp[2] & 0x80))     return -1;
    if ((resp[3] & 0x0F) != 0) return -1;

    uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
    if (ancount == 0) return -1;

    /* Skip question section */
    uint16_t off = 12;
    uint16_t qdcount = (uint16_t)((resp[4] << 8) | resp[5]);
    for (uint16_t q = 0; q < qdcount && off < (uint16_t)rlen; q++) {
        off = dns_skip_name(resp, off, (uint16_t)rlen);
        off += 4;
    }

    /* Find first PTR record */
    for (uint16_t a = 0; a < ancount && off < (uint16_t)rlen; a++) {
        off = dns_skip_name(resp, off, (uint16_t)rlen);
        if (off + 10 > (uint16_t)rlen) break;
        uint16_t rtype = (uint16_t)((resp[off]   << 8) | resp[off+1]);
        uint16_t rdlen = (uint16_t)((resp[off+8] << 8) | resp[off+9]);
        off += 10;
        if (rtype == 12) {   /* PTR */
            dns_expand_name(resp, off, (uint16_t)rlen, name_out, maxlen);
            return 0;
        }
        off += rdlen;
    }
    return -1;
}

/* ── dns_resolve_all ──────────────────────────────────────────────── */
int dns_resolve_all(const char *hostname, uint8_t ip_list[][4], int max)
{
    /* Build query */
    uint8_t  pkt[512];
    uint16_t n = 0;

    pkt[n++] = (uint8_t)(DNS_TXID >> 8);
    pkt[n++] = (uint8_t)(DNS_TXID & 0xFF);
    pkt[n++] = 0x01; pkt[n++] = 0x00;   /* RD=1 */
    pkt[n++] = 0x00; pkt[n++] = 0x01;   /* QDCOUNT = 1 */
    pkt[n++] = 0x00; pkt[n++] = 0x00;
    pkt[n++] = 0x00; pkt[n++] = 0x00;
    pkt[n++] = 0x00; pkt[n++] = 0x00;
    n += dns_encode_name(hostname, pkt + n);
    pkt[n++] = 0x00; pkt[n++] = 0x01;   /* QTYPE  = A  */
    pkt[n++] = 0x00; pkt[n++] = 0x01;   /* QCLASS = IN */

    /* Send */
    udp_open(DNS_SRC_PORT);
    if (udp_send(DNS_SERVER, DNS_SRC_PORT, DNS_PORT, pkt, n) != 0) {
        udp_close();
        return 0;
    }

    /* Poll */
    uint8_t  resp[512];
    int      rlen = 0;
    uint32_t t0   = pit_get_ticks();
    for (long iter = 0; iter < 30000000L; iter++) {
        uint8_t  buf[2048];
        uint16_t flen;
        if (e1000_recv(buf, &flen) == 0) net_recv(buf, flen);
        rlen = udp_recv(resp, (uint16_t)sizeof(resp));
        if (rlen > 0) break;
        if ((pit_get_ticks() - t0) >= DNS_TIMEOUT) break;
    }
    udp_close();

    if (rlen < 12) return 0;

    /* Validate */
    if (resp[0] != (uint8_t)(DNS_TXID >> 8) ||
        resp[1] != (uint8_t)(DNS_TXID & 0xFF)) return 0;
    if (!(resp[2] & 0x80))       return 0;   /* not a response */
    if ((resp[3] & 0x0F) != 0)   return 0;   /* RCODE != NOERROR */

    uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
    if (ancount == 0) return 0;

    /* Skip question section */
    uint16_t off = 12;
    uint16_t qdcount = (uint16_t)((resp[4] << 8) | resp[5]);
    for (uint16_t q = 0; q < qdcount && off < (uint16_t)rlen; q++) {
        off = dns_skip_name(resp, off, (uint16_t)rlen);
        off += 4;
    }

    /* Collect all A records */
    int count = 0;
    for (uint16_t a = 0; a < ancount && off < (uint16_t)rlen; a++) {
        off = dns_skip_name(resp, off, (uint16_t)rlen);
        if (off + 10 > (uint16_t)rlen) break;

        uint16_t rtype = (uint16_t)((resp[off]   << 8) | resp[off+1]);
        uint16_t rdlen = (uint16_t)((resp[off+8] << 8) | resp[off+9]);
        off += 10;

        if (rtype == 1 && rdlen == 4 && off + 4 <= (uint16_t)rlen && count < max) {
            ip_list[count][0] = resp[off];
            ip_list[count][1] = resp[off+1];
            ip_list[count][2] = resp[off+2];
            ip_list[count][3] = resp[off+3];
            count++;
        }
        off += rdlen;
    }
    return count;
}

/* ── dns_resolve — convenience wrapper ───────────────────────────── */
int dns_resolve(const char *hostname, uint8_t ip_out[4])
{
    uint8_t list[DNS_MAX_ADDRS][4];
    int n = dns_resolve_all(hostname, list, DNS_MAX_ADDRS);
    if (n <= 0) return -1;
    ip_out[0] = list[0][0]; ip_out[1] = list[0][1];
    ip_out[2] = list[0][2]; ip_out[3] = list[0][3];
    return 0;
}
