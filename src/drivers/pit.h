#ifndef PIT_H
#define PIT_H

#include "../kernel/types.h"

/* ── PIT I/O PORTS ──────────────────────────────────────────────────
   The 8253/8254 PIT exposes four I/O ports.
   All communication with the chip goes through these ports.
   ─────────────────────────────────────────────────────────────── */

#define PIT_CHANNEL0    0x40
/* Channel 0 data port — read/write the reload value for counter 0.
   Counter 0 is wired to IRQ0. This is the system timer counter.
   We write the divisor here as two bytes: low byte first, then high. */

#define PIT_CHANNEL2    0x42
/* Channel 2 data port — controls the PC speaker frequency.
   Not used in this Node but reserved for a future audio driver. */

#define PIT_COMMAND     0x43
/* Command/mode register — write-only.
   Selects which channel to configure and how to operate it.
   Must be written before writing the divisor to the data port. */

/* ── PIT COMMAND BYTE ───────────────────────────────────────────────
   The command byte sent to PIT_COMMAND encodes:
     Bits 7-6 : Channel select  (00=ch0, 01=ch1, 10=ch2, 11=read-back)
     Bits 5-4 : Access mode     (00=latch, 01=lo only, 10=hi only, 11=lo/hi)
     Bits 3-1 : Operating mode  (000–101, we use 010 = rate generator)
     Bit 0    : BCD mode        (0 = binary, 1 = BCD — we always use binary)

   0x36 = 0011 0110:
     00 = channel 0
     11 = access lo byte then hi byte
     011 = mode 3 (square wave generator)
     0 = binary counting
   ─────────────────────────────────────────────────────────────── */

#define PIT_CMD_CHANNEL0_LOHI_MODE3  0x36
/* Standard command for programming channel 0 as a square wave
   generator. Mode 3 reloads automatically — no software action
   needed between ticks. IRQ0 fires on every reload. */

/* ── FREQUENCY CONSTANTS ────────────────────────────────────────── */

#define PIT_BASE_FREQUENCY  1193182
/* The PIT's input oscillator frequency in Hz.
   1.193182 MHz — fixed by hardware on every IBM-compatible PC.
   This value has not changed since 1981. */

#define PIT_TARGET_HZ  100
/* We want 100 timer interrupts per second.
   100Hz gives 10ms resolution — sufficient for kernel scheduling.
   Each tick = 10ms of elapsed wall-clock time. */

#define PIT_DIVISOR  (PIT_BASE_FREQUENCY / PIT_TARGET_HZ)
/* = 1193182 / 100 = 11931 (integer division, ~99.97Hz actual).
   Written to channel 0: low byte first, then high byte.
   The PIT counts down from this value at 1.193182 MHz,
   firing IRQ0 each time it reaches zero and reloading. */

/* ── PUBLIC INTERFACE ───────────────────────────────────────────── */

void     pit_init(void);
void     irq_install(uint8_t irq, void (*handler)(void));
/* Register a C handler for the given IRQ number (0–15).
   Called by keyboard_init and any future driver needing an IRQ. */
/* Program the PIT to fire at PIT_TARGET_HZ.
   Unmask IRQ0 at the PIC master.
   Must be called after idt_init — the IRQ0 gate must exist. */

uint32_t pit_get_ticks(void);
/* Return the current tick count since pit_init was called.
   Each tick = 1/PIT_TARGET_HZ seconds = 10ms at 100Hz.
   Used by future sleep() and scheduler timeout logic. */

void irq_handler(uint32_t irq_num);
/* C-level IRQ dispatch called by irq_common_stub in irq.asm.
   Routes each IRQ number to its registered handler.
   Sends EOI to the PIC before returning. */

#endif
