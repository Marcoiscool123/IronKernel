#ifndef DHCP_H
#define DHCP_H

/* Run DHCP DORA (Discover→Offer→Request→ACK).
   On success, updates g_net_ip, g_net_gw, g_net_dns in ip.c.
   Returns 0 on success, -1 on timeout or no response. */
int dhcp_discover(void);

#endif
