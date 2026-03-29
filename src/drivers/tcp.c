/* IronKernel — tcp.c
   Minimal TCP client + HTTP/1.0 client.
   Single-connection, polled (no interrupt-driven RX).
   Guest: 10.0.2.15  Gateway: 10.0.2.2  (QEMU SLIRP defaults) */

#include "tcp.h"
#include "net.h"
#include "ip.h"
#include "e1000.h"
#include "vga.h"
#include "../drivers/pit.h"

/* Guest IP comes from the global network config in ip.c */

/* ── ISN generator — LCG seeded from PIT ticks ───────────────────── */
static uint32_t g_isn_state = 0;

static uint32_t tcp_gen_isn(void)
{
    /* Seed once from PIT ticks (time since boot gives per-boot entropy) */
    if (!g_isn_state)
        g_isn_state = pit_get_ticks() | 1u;

    /* Numerical Recipes LCG: good distribution, cheap */
    g_isn_state = g_isn_state * 1664525u + 1013904223u;
    uint32_t isn = g_isn_state;

    /* Debug: print ISN as 8 hex digits */
    static const char hx[] = "0123456789ABCDEF";
    char buf[9]; buf[8] = '\0';
    uint32_t tmp = isn;
    for (int i = 7; i >= 0; i--) { buf[i] = hx[tmp & 0xFu]; tmp >>= 4; }
    vga_print("[TCP] ISN: 0x");
    vga_print(buf);
    vga_print("\n");

    return isn;
}

/* ── Connection state ────────────────────────────────────────────── */
#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2

static struct {
    uint8_t  dst_ip[4];
    uint16_t dst_port;
    uint16_t src_port;
    uint32_t seq;       /* next SEQ we will send */
    uint32_t ack;       /* next SEQ we expect from remote */
    int      state;
} g_tcp;

/* ── Receive buffer ──────────────────────────────────────────────── */
#define TCP_RX_SIZE 8192
static uint8_t  g_rx_buf[TCP_RX_SIZE];
static uint32_t g_rx_len;
static int      g_rx_fin;   /* 1 once a FIN is received */

/* ── Retransmission buffer ───────────────────────────────────────── */
#define TCP_RTO_TICKS  100u  /* 1 s at 100 Hz PIT before retransmit  */
#define TCP_MAX_RETX   5     /* give up after 5 retransmissions       */

static struct {
    uint8_t  seg[20 + 1460]; /* full TCP segment awaiting ACK         */
    uint16_t seg_len;
    uint32_t seq_end;        /* remote must ACK up to here to clear   */
    uint32_t sent_tick;
    int      active;
    int      retries;
} g_retx;

/* ── TCP checksum (pseudo-header + segment) ──────────────────────── */
static uint16_t tcp_cksum(const uint8_t *seg, uint16_t tcp_len)
{
    uint32_t sum = 0;

    /* Pseudo-header: each pair of IP octets forms one 16-bit big-endian word */
    sum += ((uint32_t)g_net_ip[0] << 8) | g_net_ip[1];
    sum += ((uint32_t)g_net_ip[2] << 8) | g_net_ip[3];
    sum += ((uint32_t)g_tcp.dst_ip[0] << 8) | g_tcp.dst_ip[1];
    sum += ((uint32_t)g_tcp.dst_ip[2] << 8) | g_tcp.dst_ip[3];
    sum += 6;                               /* Protocol 6 (TCP) */
    sum += tcp_len;                         /* TCP length */

    /* TCP segment bytes */
    const uint8_t *p = seg;
    uint16_t rem = tcp_len;
    while (rem > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; rem -= 2; }
    if (rem)         sum += (uint32_t)p[0] << 8;

    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ── Build and send one TCP segment ──────────────────────────────── */
static int tcp_tx(uint8_t flags, const void *data, uint16_t dlen)
{
    uint16_t tcp_len = 20u + dlen;
    if (tcp_len > 1460u + 20u) return -1;

    uint8_t seg[20 + 1460];

    tcp_hdr_t *h  = (tcp_hdr_t *)seg;
    h->src_port   = htons(g_tcp.src_port);
    h->dst_port   = htons(g_tcp.dst_port);
    h->seq        = htonl(g_tcp.seq);
    h->ack_seq    = (flags & TCP_ACK) ? htonl(g_tcp.ack) : 0;
    h->data_off   = 0x50;          /* 5 × 4 = 20-byte header, no options */
    h->flags      = flags;
    h->window     = htons(4096);
    h->checksum   = 0;
    h->urgent     = 0;

    const uint8_t *src = (const uint8_t *)data;
    for (uint16_t i = 0; i < dlen; i++) seg[20 + i] = src[i];

    h->checksum = htons(tcp_cksum(seg, tcp_len));

    int r = ip_send(g_tcp.dst_ip, IP_PROTO_TCP, seg, tcp_len);

    /* Arm retransmission for segments carrying payload (data loss matters) */
    if (r == 0 && dlen > 0) {
        for (uint16_t i = 0; i < tcp_len; i++) g_retx.seg[i] = seg[i];
        g_retx.seg_len   = tcp_len;
        g_retx.seq_end   = g_tcp.seq + dlen;
        g_retx.sent_tick = pit_get_ticks();
        g_retx.active    = 1;
        g_retx.retries   = 0;
    }
    return r;
}

/* ── tcp_recv_frame — called from net_recv ───────────────────────── */
void tcp_recv_frame(const uint8_t *frame, uint16_t len)
{
    if (g_tcp.state == TCP_CLOSED) return;
    if (len < 14u + 20u + 20u) return;

    const ip_hdr_t  *ip  = (const ip_hdr_t  *)(frame + 14);
    const tcp_hdr_t *tcp = (const tcp_hdr_t *)(frame + 14 + 20);

    if (ip->protocol != IP_PROTO_TCP) return;
    if (ntohs(tcp->src_port) != g_tcp.dst_port) return;
    if (ntohs(tcp->dst_port) != g_tcp.src_port) return;
    for (int i = 0; i < 4; i++)
        if (ip->src_ip[i] != g_tcp.dst_ip[i]) return;

    uint8_t  flags   = tcp->flags;
    uint32_t seg_seq = ntohl(tcp->seq);

    /* RST — abort */
    if (flags & TCP_RST) { g_tcp.state = TCP_CLOSED; return; }

    /* SYN+ACK during handshake */
    if (g_tcp.state == TCP_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            g_tcp.ack   = seg_seq + 1u;
            g_tcp.state = TCP_ESTABLISHED;
            tcp_tx(TCP_ACK, 0, 0);          /* complete handshake */
        }
        return;
    }

    /* Data / FIN in ESTABLISHED */
    if (g_tcp.state == TCP_ESTABLISHED) {
        /* ACK from remote — clear retx buffer if our data was delivered */
        if ((flags & TCP_ACK) && g_retx.active &&
            ntohl(tcp->ack_seq) >= g_retx.seq_end)
            g_retx.active = 0;
        uint8_t  hdr_bytes = (uint8_t)((tcp->data_off >> 4) * 4u);
        uint16_t ip_total  = ntohs(ip->total_len);
        uint16_t tcp_total = (ip_total > 20u) ? (uint16_t)(ip_total - 20u) : 0u;
        uint16_t data_len  = (tcp_total > hdr_bytes) ? (uint16_t)(tcp_total - hdr_bytes) : 0u;

        const uint8_t *data = (const uint8_t *)tcp + hdr_bytes;

        /* Store received bytes */
        if (data_len > 0) {
            for (uint16_t i = 0; i < data_len && g_rx_len < TCP_RX_SIZE; i++)
                g_rx_buf[g_rx_len++] = data[i];
            g_tcp.ack = seg_seq + data_len;
            tcp_tx(TCP_ACK, 0, 0);
        }

        /* FIN from server */
        if (flags & TCP_FIN) {
            g_tcp.ack = seg_seq + data_len + 1u;
            tcp_tx(TCP_ACK | TCP_FIN, 0, 0);
            g_tcp.seq++;
            g_rx_fin        = 1;
            g_tcp.state     = TCP_CLOSED;
        }
    }
}

/* ── Retransmission check — call from every polling loop ─────────── */
static void retx_check(void)
{
    if (!g_retx.active) return;
    if (g_tcp.state != TCP_ESTABLISHED) { g_retx.active = 0; return; }
    if ((pit_get_ticks() - g_retx.sent_tick) < TCP_RTO_TICKS) return;

    if (++g_retx.retries > TCP_MAX_RETX) {
        g_tcp.state   = TCP_CLOSED;
        g_retx.active = 0;
        return;
    }

    /* Update ACK field to current value then recompute checksum */
    tcp_hdr_t *h = (tcp_hdr_t *)g_retx.seg;
    h->ack_seq  = htonl(g_tcp.ack);
    h->checksum = 0;
    h->checksum = htons(tcp_cksum(g_retx.seg, g_retx.seg_len));
    ip_send(g_tcp.dst_ip, IP_PROTO_TCP, g_retx.seg, g_retx.seg_len);
    g_retx.sent_tick = pit_get_ticks();
}

/* ── Poll helper: pump e1000 → net_recv ──────────────────────────── */
static void pump(void)
{
    uint8_t  buf[2048];
    uint16_t rlen;
    if (e1000_recv(buf, &rlen) == 0) net_recv(buf, rlen);
    retx_check();
}

/* ── tcp_connect ─────────────────────────────────────────────────── */
int tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port)
{
    for (int i = 0; i < 4; i++) g_tcp.dst_ip[i] = dst_ip[i];
    g_tcp.dst_port = dst_port;
    static uint16_t s_next_port = 49152;
    g_tcp.src_port = s_next_port++;
    if (s_next_port == 0 || s_next_port < 49152) s_next_port = 49152;
    uint32_t syn_seq = tcp_gen_isn();
    g_tcp.seq      = syn_seq;   /* SYN must carry ISN — do NOT pre-increment */
    g_tcp.ack      = 0;
    g_tcp.state    = TCP_SYN_SENT;
    g_rx_len       = 0;
    g_rx_fin       = 0;

    if (tcp_tx(TCP_SYN, 0, 0) != 0) { g_tcp.state = TCP_CLOSED; return -1; }
    g_tcp.seq = syn_seq + 1u;   /* SYN consumes one sequence number */

    uint32_t t0    = pit_get_ticks();
    uint32_t t_syn = t0;
    for (long i = 0; i < 50000000L; i++) {
        pump();
        if (g_tcp.state == TCP_ESTABLISHED) return 0;
        if (g_tcp.state == TCP_CLOSED)      return -1;
        /* Retransmit SYN: restore ISN (seq must match original SYN) */
        if ((pit_get_ticks() - t_syn) >= TCP_RTO_TICKS) {
            g_tcp.seq = syn_seq;
            tcp_tx(TCP_SYN, 0, 0);
            g_tcp.seq = syn_seq + 1u;
            t_syn = pit_get_ticks();
        }
        if ((pit_get_ticks() - t0) >= 500u) break;  /* 5 s total */
    }
    g_tcp.state = TCP_CLOSED;
    return -1;
}

/* ── tcp_send_data ───────────────────────────────────────────────── */
int tcp_send_data(const void *data, uint16_t len)
{
    if (g_tcp.state != TCP_ESTABLISHED) return -1;
    int r = tcp_tx(TCP_PSH | TCP_ACK, data, len);
    if (r == 0) g_tcp.seq += len;
    return r;
}

/* ── tcp_recv_data ───────────────────────────────────────────────── */
int tcp_recv_data(void *buf, uint32_t maxlen, uint32_t timeout_ticks)
{
    uint32_t t0 = pit_get_ticks();
    for (long i = 0; i < 200000000L; i++) {
        pump();
        if (g_rx_fin || g_tcp.state == TCP_CLOSED) break;
        if ((pit_get_ticks() - t0) >= timeout_ticks) break;
    }
    uint32_t copy = (g_rx_len < maxlen) ? g_rx_len : maxlen;
    uint8_t *dst  = (uint8_t *)buf;
    for (uint32_t i = 0; i < copy; i++) dst[i] = g_rx_buf[i];
    return (int)copy;
}

/* ── tcp_close ───────────────────────────────────────────────────── */
void tcp_close(void)
{
    if (g_tcp.state == TCP_ESTABLISHED) {
        tcp_tx(TCP_FIN | TCP_ACK, 0, 0);
        g_tcp.seq++;
    }
    g_tcp.state = TCP_CLOSED;
}

/* ── http_get ────────────────────────────────────────────────────── */
int http_get(const uint8_t ip[4], uint16_t port,
             const char *host, const char *path,
             uint8_t *buf, uint32_t maxlen)
{
    /* SLIRP cold-start: on first TCP connection after boot, QEMU's SLIRP
       stack needs time to set up its real OS socket and connect to the
       remote host.  Retry up to 3 times — typically succeeds by attempt 2. */
    int attempt;
    for (attempt = 0; attempt < 3; attempt++) {
        if (tcp_connect(ip, port) == 0) break;
    }
    if (attempt == 3) return -1;

    /* Build "GET /path HTTP/1.0\r\nHost: hostname\r\nConnection: close\r\n\r\n" */
    uint8_t req[512];
    uint16_t n = 0;

    const char *p;
    for (p = "GET ";          *p; p++) req[n++] = (uint8_t)*p;
    for (p = path;            *p; p++) req[n++] = (uint8_t)*p;
    for (p = " HTTP/1.0\r\nHost: "; *p; p++) req[n++] = (uint8_t)*p;
    for (p = host;            *p; p++) req[n++] = (uint8_t)*p;
    for (p = "\r\nConnection: close\r\n\r\n"; *p; p++) req[n++] = (uint8_t)*p;

    if (tcp_send_data(req, n) != 0) { tcp_close(); return -1; }

    /* Receive full response (10 s timeout) — static to avoid stack overflow */
    static uint8_t raw[TCP_RX_SIZE];
    int total = tcp_recv_data(raw, TCP_RX_SIZE, 1000u);
    tcp_close();

    if (total <= 0) return -1;

    /* Skip HTTP headers — find \r\n\r\n */
    int body_start = 0;
    for (int i = 0; i < total - 3; i++) {
        if (raw[i]   == '\r' && raw[i+1] == '\n' &&
            raw[i+2] == '\r' && raw[i+3] == '\n') {
            body_start = i + 4;
            break;
        }
    }

    uint32_t body_len = (uint32_t)(total - body_start);
    if (body_len > maxlen) body_len = maxlen;
    for (uint32_t i = 0; i < body_len; i++) buf[i] = raw[body_start + i];
    return (int)body_len;
}
