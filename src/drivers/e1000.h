#ifndef E1000_H
#define E1000_H

#include "../kernel/types.h"

/* ── Intel 82540EM (e1000) driver API ───────────────────────────── */

int  e1000_init(void);
/* Locate the e1000 PCI device, map its MMIO BAR, program TX/RX rings,
   and install the IRQ handler.  Returns 0 on success, -1 if not found. */

int  e1000_send(const void *data, uint16_t len);
/* Transmit a raw Ethernet frame (including Ethernet header).
   Busy-waits for the TX descriptor slot to be free.
   Returns 0 on success, -1 if driver not ready or frame too large. */

int  e1000_recv(void *buf, uint16_t *len_out);
/* Poll for a received frame.  Copies at most 2048 bytes into buf.
   Returns 0 and sets *len_out on success, -1 if no frame available. */

void e1000_mac(uint8_t mac[6]);
/* Copy the device MAC address into mac[]. */

int  e1000_ready(void);
/* Returns 1 if driver was successfully initialised, 0 otherwise. */

int  e1000_link_up(void);
/* Returns 1 if the physical link is up (STATUS.LU set). */

uint32_t e1000_reg(uint32_t offset);
/* Read any MMIO register by byte offset (for diagnostics). Returns 0 if not ready. */

/* Commonly-used offsets for diagnostics */
#define E1000_OFF_STATUS  0x0008u
#define E1000_OFF_RDH     0x2810u
#define E1000_OFF_TDH     0x3810u
#define E1000_OFF_ICR     0x00C0u

#endif
