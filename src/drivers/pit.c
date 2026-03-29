#include "pit.h"
#include "../kernel/sched.h"
#include "vga.h"
#include "../kernel/types.h"

/* ── PORT I/O ───────────────────────────────────────────────────── */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %1, %0" : : "dN"(port), "a"(val));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

/* ── PIC PORTS — needed to send EOI and unmask IRQ0 ─────────────── */

#define PIC1_COMMAND  0x20
#define PIC1_DATA     0x21
#define PIC2_COMMAND  0xA0
#define PIC_EOI       0x20

/* ── TICK COUNTER ───────────────────────────────────────────────── */

static volatile uint32_t pit_ticks = 0;
/* volatile: the compiler must not cache this value in a register.
   It is modified inside an interrupt handler — the main execution
   path must always read the current value from memory, not from
   a register that was loaded before the interrupt fired. */

/* ── IRQ HANDLER TABLE ──────────────────────────────────────────── */

#define IRQ_COUNT 16

typedef void (*irq_handler_t)(void);

static irq_handler_t irq_handlers[IRQ_COUNT];
/* Function pointer table — one slot per IRQ (0–15).
   Null = no handler registered for that IRQ.
   pit_init installs the timer handler in slot 0.
   Future drivers install their handlers here via irq_install. */



/* ── TIMER HANDLER ──────────────────────────────────────────────── */

static void timer_callback(void)
{
    pit_ticks++;
    sched_tick();
    /* Increment tick counter on every IRQ0 firing.
       At 100Hz this increments 100 times per second.
       pit_ticks / 100 = elapsed seconds since pit_init. */
}

/* ── C IRQ DISPATCHER ───────────────────────────────────────────── */



/* ── PIT INIT ───────────────────────────────────────────────────── */

void pit_init(void)
{
    /* Zero the handler table. */
    for (int i = 0; i < IRQ_COUNT; i++) irq_handlers[i] = 0;

    /* Register our timer callback for IRQ0. */
    irq_install(0, timer_callback);

    /* Program PIT channel 0. */
    outb(PIT_COMMAND, PIT_CMD_CHANNEL0_LOHI_MODE3);
    /* Command byte: channel 0, lo/hi access, mode 3 (square wave),
       binary. Must be sent before writing the divisor. */

    outb(PIT_CHANNEL0, (uint8_t)(PIT_DIVISOR & 0xFF));
    /* Low byte of divisor first. */

    outb(PIT_CHANNEL0, (uint8_t)((PIT_DIVISOR >> 8) & 0xFF));
    /* High byte of divisor second.
       PIT now counts down from 11931 at 1.193182 MHz,
       firing IRQ0 at approximately 100Hz. */

    /* Unmask IRQ0 at the master PIC.
       During idt_init we masked all IRQs (0xFF).
       Now we selectively unmask only IRQ0 — bit 0 of PIC1_DATA.
       All other IRQs remain masked until their drivers are ready. */
    uint8_t mask = inb(PIC1_DATA);
    mask &= ~(1 << 0);
    /* Clear bit 0: unmask IRQ0 (timer).
       All other bits unchanged — all other IRQs stay masked. */
    outb(PIC1_DATA, mask);

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("[PIT] TIMER ONLINE\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("      Frequency : 100 Hz\n");
    vga_print("      Divisor   : 11931\n");
    vga_print("      IRQ0      : UNMASKED\n");
}

uint32_t pit_get_ticks(void) { return pit_ticks; }
