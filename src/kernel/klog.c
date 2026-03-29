/* IronKernel — klog.c
   Ring buffer kernel logger.  Entries are stored in BSS with no
   dynamic allocation.  Safe to call at any point during boot.     */

#include "klog.h"
#include "types.h"
#include "../drivers/vga.h"
#include "../drivers/pit.h"
#include "../drivers/serial.h"

/* ── Ring buffer ─────────────────────────────────────────────────── */

#define KLOG_CAP     2048   /* must be a power of 2                 */
#define KLOG_MSGLEN   120   /* max message chars (incl. null)       */
/* 2048 * (4+1+120) = 256,000 bytes ≈ 250 KB in BSS               */

typedef struct {
    uint32_t ticks;          /* PIT ticks at log time (100 Hz)     */
    uint8_t  level;          /* LOG_* constant                     */
    char     msg[KLOG_MSGLEN];
} klog_entry_t;

static klog_entry_t klog_ring[KLOG_CAP];
static uint32_t     klog_write = 0;   /* next slot to write         */
static uint32_t     klog_count = 0;   /* entries stored so far      */

/* ── helpers ─────────────────────────────────────────────────────── */

static void k_strcpy(char *dst, const char *src, int max)
{
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Write a right-aligned decimal number into buf[width], no null.
   buf must have at least width bytes. */
static void fmt_uint_w(char *buf, uint32_t v, int width)
{
    for (int i = width - 1; i >= 0; i--) {
        buf[i] = '0' + (v % 10);
        v /= 10;
        if (!v) {
            /* left-pad with spaces */
            for (int j = i - 1; j >= 0; j--) buf[j] = ' ';
            return;
        }
    }
}

/* Write a zero-padded decimal into buf[width], no null. */
static void fmt_uint_z(char *buf, uint32_t v, int width)
{
    for (int i = width - 1; i >= 0; i--) {
        buf[i] = '0' + (v % 10);
        v /= 10;
    }
}

/* ── klog ─────────────────────────────────────────────────────────── */

void klog(uint8_t level, const char *msg)
{
    /* Disable interrupts for the write to avoid a race with IRQ handlers
       that might also call klog (e.g. ELF crash in isr_handler). */
    __asm__ volatile("cli");

    klog_entry_t *e = &klog_ring[klog_write & (KLOG_CAP - 1)];
    e->ticks = (uint32_t)pit_get_ticks();
    e->level = level;
    k_strcpy(e->msg, msg, KLOG_MSGLEN);

    klog_write = (klog_write + 1) & (KLOG_CAP - 1);
    if (klog_count < KLOG_CAP) klog_count++;

    __asm__ volatile("sti");

    /* Mirror to COM1 serial: "[SSSS.CC] [LEVEL] message\r\n" */
    static const char *lvl[4] = { "INFO", "WARN", "ERR!", "PNC!" };
    char ts[12];
    ts[0] = '[';
    uint32_t sec = e->ticks / 100, cs = e->ticks % 100;
    fmt_uint_w(ts + 1, sec, 4);
    ts[5] = '.';
    fmt_uint_z(ts + 6, cs, 2);
    ts[8] = ']'; ts[9] = ' '; ts[10] = '\0';
    serial_puts(ts);
    serial_puts("[");
    serial_puts(lvl[level < 4 ? level : 0]);
    serial_puts("] ");
    serial_puts(msg);
    serial_puts("\r\n");
}

/* ── klog_dump ────────────────────────────────────────────────────── */

void klog_dump(void)
{
    if (klog_count == 0) {
        vga_print("  (log empty)\n");
        return;
    }

    /* Oldest entry is klog_count slots behind the write pointer. */
    uint32_t start = (klog_write - klog_count) & (KLOG_CAP - 1);

    /* Timestamp buffer: "[ssss.cc] " = 10 chars + null */
    char ts[12];
    ts[0] = '['; ts[10] = '\0'; ts[11] = '\0';
    /* Format: "[SSSS.CC]" */

    static const char *level_str[4] = { "INFO", "WARN", "ERR!", "PNC!" };

    for (uint32_t i = 0; i < klog_count; i++) {
        klog_entry_t *e = &klog_ring[(start + i) & (KLOG_CAP - 1)];

        /* Build timestamp "[SSSS.CC]" */
        uint32_t sec = e->ticks / 100;
        uint32_t cs  = e->ticks % 100;
        fmt_uint_w(ts + 1, sec, 4);
        ts[5] = '.';
        fmt_uint_z(ts + 6, cs, 2);
        ts[8] = ']';
        ts[9] = ' ';

        /* Color: timestamp always dark grey */
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_print(ts);

        /* Level tag with color */
        switch (e->level) {
        case LOG_WARN:
            vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
            break;
        case LOG_ERROR:
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            break;
        case LOG_PANIC:
            vga_set_color(VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK);
            break;
        default:
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            break;
        }
        vga_print("[");
        vga_print(level_str[e->level < 4 ? e->level : 0]);
        vga_print("] ");

        /* Message */
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_print(e->msg);
        vga_print("\n");
    }

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}
