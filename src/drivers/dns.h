#ifndef DNS_H
#define DNS_H

#include "../kernel/types.h"

/* DNS server used for all queries — SLIRP provides DNS forwarding at 10.0.2.3 */
#define DNS_SERVER_IP { 10, 0, 2, 3 }

/* Maximum A records collected by a single query */
#define DNS_MAX_ADDRS 8

/* Resolve all A records for a hostname.
   Fills ip_list[0..n-1][4] and returns the count (0 on failure).
   ip_list must point to at least DNS_MAX_ADDRS × 4 bytes. */
int dns_resolve_all(const char *hostname, uint8_t ip_list[][4], int max);

/* Convenience wrapper — returns just the first A record.
   Returns 0 on success, -1 on failure. */
int dns_resolve(const char *hostname, uint8_t ip_out[4]);

/* Reverse DNS lookup — queries the PTR record for an IPv4 address.
   ip[4] is in host order (e.g. {8,8,8,8}).
   Fills name_out with the hostname string (NUL-terminated, up to maxlen bytes).
   Returns 0 on success, -1 on failure or timeout. */
int dns_reverse(const uint8_t ip[4], char *name_out, uint16_t maxlen);

#endif
