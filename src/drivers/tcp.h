#ifndef TCP_H
#define TCP_H

#include "../kernel/types.h"

/* ── Minimal TCP client ──────────────────────────────────────────── */

int  tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port);
/* Open a TCP connection to dst_ip:dst_port (3-way handshake).
   Returns 0 on success, -1 on timeout or error. */

int  tcp_send_data(const void *data, uint16_t len);
/* Send len bytes over an established connection.
   Returns 0 on success, -1 if not connected. */

int  tcp_recv_data(void *buf, uint32_t maxlen, uint32_t timeout_ticks);
/* Receive data until FIN or timeout.  Returns byte count received.
   timeout_ticks: PIT ticks to wait (100 ticks = 1 s). */

void tcp_close(void);
/* Send FIN and mark connection closed. */

void tcp_recv_frame(const uint8_t *frame, uint16_t len);
/* Called by net_recv to deliver received Ethernet frames to TCP. */

/* ── HTTP/1.0 client ─────────────────────────────────────────────── */

int  http_get(const uint8_t ip[4], uint16_t port,
              const char *host, const char *path,
              uint8_t *buf, uint32_t maxlen);
/* Send GET request and receive response body into buf.
   host is used for the HTTP Host: header (must be the hostname string).
   Returns number of body bytes written, -1 on error. */

#endif
