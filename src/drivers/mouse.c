/*
 * mouse.c — PS/2 auxiliary (mouse) driver for IronKernel.
 *
 * Polls the 8042 keyboard controller for mouse packets.
 * Mouse packets are 3 bytes; byte 0 has bit 3 always set (sync).
 * Y axis is inverted: positive dy = cursor moves UP on screen.
 *
 * No IRQ12 handler — polled from the GUI main loop.
 */
#include "mouse.h"
#include "vga.h"

/* ── Globals ──────────────────────────────────────────────────────── */
int     mouse_x     = 400;
int     mouse_y     = 300;
uint8_t mouse_btn   = 0;
int     mouse_moved = 0;

/* ── I/O helpers ─────────────────────────────────────────────────── */
static inline void outb(uint16_t p, uint8_t v)
{ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

static inline uint8_t inb(uint16_t p)
{ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

static void wait_wr(void) { while (inb(0x64) & 0x02); }
static void wait_rd(void) { int t=200000; while (!(inb(0x64)&1) && t-->0); }

static void mouse_cmd(uint8_t b)
{ wait_wr(); outb(0x64,0xD4); wait_wr(); outb(0x60,b); }

static uint8_t mouse_rd(void) { wait_rd(); return inb(0x60); }

/* ── mouse_disable ───────────────────────────────────────────────── */
void mouse_disable(void)
{
    /* Stop the mouse from sending packets so the 8042 output buffer
       does not stay full after the WM exits.  A full OBF blocks keyboard
       scancodes from being placed in the buffer, preventing IRQ1 from
       ever firing — which is why the text shell can't receive input
       after exiting the WM via mouse click. */
    mouse_cmd(0xF5); mouse_rd();   /* disable streaming / ACK */

    /* Drain only mouse bytes sitting in the output buffer.
       Status bit 5 (0x20) distinguishes mouse (1) from keyboard (0).
       Never read keyboard bytes here — that corrupts the keyboard
       driver's shift/caps state and scrambles subsequent input. */
    for (int i = 0; i < 16; i++) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) break;        /* OBF clear — nothing left  */
        if (!(st & 0x20)) break;        /* keyboard byte — leave it  */
        inb(0x60);                      /* mouse byte — discard      */
    }
}

/* ── mouse_init ──────────────────────────────────────────────────── */
void mouse_init(void)
{
    /* Enable aux device */
    wait_wr(); outb(0x64, 0xA8);

    /* Read + patch controller command byte.
       Bit 0 (0x01) = keyboard interrupt enable — must stay set.
       Bit 1 (0x02) = aux interrupt enable     — set for mouse.
       Bit 4 (0x10) = keyboard clock disable   — clear to keep KB alive.
       Bit 5 (0x20) = aux clock disable        — clear to enable mouse. */
    wait_wr(); outb(0x64, 0x20);
    wait_rd();
    uint8_t cb = (inb(0x60) | 0x03) & ~0x30u;
    wait_wr(); outb(0x64, 0x60);
    wait_wr(); outb(0x60, cb);

    /* Mouse: set defaults, sample rate 40, enable streaming */
    mouse_cmd(0xF6); mouse_rd();   /* set defaults / ACK */
    mouse_cmd(0xF3); mouse_rd();   /* set sample rate    */
    mouse_cmd(200);  mouse_rd();   /* 200 sps            */
    mouse_cmd(0xF4); mouse_rd();   /* enable streaming   */

    mouse_x = 400; mouse_y = 300;
    mouse_btn = 0; mouse_moved = 0;
}

/* ── mouse_poll ──────────────────────────────────────────────────── */
void mouse_poll(void)
{
    static int     phase = 0;
    static uint8_t pkt[3];
    static uint8_t prev_btn = 0;
    static int prev_x = 400, prev_y = 300;

    while (1) {
        uint8_t st = inb(0x64);
        if (!(st & 0x01)) break;          /* output buffer empty */
        if (!(st & 0x20)) {               /* keyboard data — leave it for IRQ handler */
            break;
        }
        uint8_t b = inb(0x60);

        /* Phase 0: wait for sync byte (bit 3 must be set) */
        if (phase == 0 && !(b & 0x08)) continue;

        pkt[phase++] = b;
        if (phase < 3) continue;
        phase = 0;

        uint8_t flags = pkt[0];

        /* Ignore if overflow bits set */
        if (flags & 0xC0) continue;

        /* PS/2 uses 9-bit two's complement: bit4/5 of flags are the MSBs.
           Casting pkt[1] directly to int8_t only works for |delta| < 128.
           For fast movement (128..255), bit7 of pkt[1] is set but bit4 says
           positive — int8_t flips the sign and the cursor flies backwards. */
        int dx = (int)pkt[1];
        if (flags & 0x10) dx -= 256;
        int dy = (int)pkt[2];
        if (flags & 0x20) dy -= 256;

        mouse_x += dx;
        mouse_y -= dy;           /* screen Y is top-down */

        if (mouse_x < 0)   mouse_x = 0;
        if (mouse_x > 799) mouse_x = 799;
        if (mouse_y < 0)   mouse_y = 0;
        if (mouse_y > 599) mouse_y = 599;

        mouse_btn = flags & 0x07;
    }

    mouse_moved = (mouse_x != prev_x || mouse_y != prev_y ||
                   mouse_btn != prev_btn);
    prev_x = mouse_x; prev_y = mouse_y; prev_btn = mouse_btn;
}
