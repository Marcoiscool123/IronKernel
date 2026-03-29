#ifndef ARP_H
#define ARP_H

#include "../kernel/types.h"

/* ── ARP layer ───────────────────────────────────────────────────── */

/* Resolve an IPv4 address to its Ethernet MAC.
   Sends an ARP request and polls for a reply (up to 3 s timeout).
   Returns 0 and fills mac[6] on success, -1 on timeout. */
int  arp_resolve(const uint8_t ip[4], uint8_t mac[6]);

/* Process a received Ethernet frame that may contain an ARP packet.
   Updates the ARP cache; replies to requests directed at our IP.
   Called by net_recv() and by the arp_resolve() polling loop. */
void arp_recv(const uint8_t *frame, uint16_t len);

#endif
