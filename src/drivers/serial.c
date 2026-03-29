/* IronKernel — serial.c
   COM1 (I/O base 0x3F8) debug-output driver.
   115200 baud, 8 data bits, no parity, 1 stop bit.
   Polled TX — no interrupt-driven output, no RX.

   QEMU usage:  add  -serial file:serial.log  to the run target, or
                use  -serial stdio  to see output in the terminal.     */

#include "serial.h"
#include "../kernel/types.h"

#define COM1  0x3F8u    /* standard COM1 base port */

/* ── I/O helpers ──────────────────────────────────────────────────── */
static inline void outb(uint16_t port, uint8_t val)
{ __asm__ volatile("outb %0,%1" :: "a"(val), "Nd"(port)); }

static inline uint8_t inb(uint16_t port)
{ uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }

/* ── serial_init ──────────────────────────────────────────────────── */
void serial_init(void)
{
    outb(COM1 + 1, 0x00);  /* disable all UART interrupts              */
    outb(COM1 + 3, 0x80);  /* set DLAB so offsets 0/1 address divisor  */
    outb(COM1 + 0, 0x01);  /* divisor low  byte: 1  →  115200 baud     */
    outb(COM1 + 1, 0x00);  /* divisor high byte: 0                     */
    outb(COM1 + 3, 0x03);  /* clear DLAB; 8 data bits, no parity, 1 stop */
    outb(COM1 + 2, 0xC7);  /* enable FIFO; clear TX/RX; 14-byte trigger*/
    outb(COM1 + 4, 0x0B);  /* RTS + DTR asserted; IRQ pin enabled      */
}

/* ── serial_putchar ───────────────────────────────────────────────── */
void serial_putchar(char c)
{
    /* Spin until the Transmit Holding Register is empty (LSR bit 5). */
    int guard = 100000;
    while (!(inb(COM1 + 5) & 0x20) && guard-- > 0);
    outb(COM1, (uint8_t)c);
}

/* ── serial_puts ──────────────────────────────────────────────────── */
void serial_puts(const char *s)
{
    while (*s) serial_putchar(*s++);
}
