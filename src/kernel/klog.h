#ifndef KLOG_H
#define KLOG_H
#include "types.h"

/* ── KERNEL LOG LEVELS ───────────────────────────────────────────── */
#define LOG_INFO   0   /* normal status messages                     */
#define LOG_WARN   1   /* non-fatal anomalies                        */
#define LOG_ERROR  2   /* errors that impair functionality           */
#define LOG_PANIC  3   /* logged just before a kernel panic          */

/* ── PUBLIC API ──────────────────────────────────────────────────── */

/* Store a message in the kernel ring buffer.
   Does NOT print to VGA — use alongside existing vga_print calls.
   Safe to call before PIT init (timestamp will be 0). */
void klog(uint8_t level, const char *msg);

/* Dump the full ring buffer to the VGA console (for dmesg command).
   Color-codes each entry by level. */
void klog_dump(void);

#endif
