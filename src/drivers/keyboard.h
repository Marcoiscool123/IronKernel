#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "../kernel/types.h"

/* ── PS/2 I/O PORTS ─────────────────────────────────────────────────
   The 8042 keyboard controller exposes two ports.
   These have been at the same addresses since the 1981 IBM PC.
   ─────────────────────────────────────────────────────────────── */

#define KB_DATA_PORT    0x60
/* Read: receive scancode byte from keyboard controller buffer.
   The IRQ1 handler reads this port once per interrupt to get
   the scancode for the key that was just pressed or released. */

#define KB_STATUS_PORT  0x64
/* Read:  status register — bit 0 set = output buffer full (data ready).
   Write: command register — send commands to the 8042 controller.
   We do not need to poll the status port in interrupt-driven mode —
   the interrupt itself signals that data is ready at port 0x60. */

/* ── SCANCODE CONSTANTS ─────────────────────────────────────────── */

#define KB_RELEASE_BIT  0x80
/* If bit 7 of a scancode is set, it is a break code (key release).
   The corresponding make code is scancode & ~KB_RELEASE_BIT.
   We ignore break codes — we only act on key presses. */

#define KB_LSHIFT       0x2A
#define KB_RSHIFT       0x36
/* Left and right Shift make codes.
   When either is held, we use the shifted scancode table.
   We track shift state in a static flag in keyboard.c. */

#define KB_CAPS         0x3A
/* CapsLock make code. Toggles the caps_lock flag on each press.
   Affects only alphabetic keys — does not shift symbols. */

#define KB_BUFFER_SIZE  256

/* Special non-ASCII key codes placed in the ring buffer.
   Values 0x01–0x02 are control codes never produced by normal typing. */
#define KEY_UP    0x01
#define KEY_DOWN  0x02
#define KEY_LEFT  0x80
#define KEY_RIGHT 0x81
/* Ring buffer capacity — 256 bytes.
   Power of 2: head/tail wrap using & (KB_BUFFER_SIZE - 1).
   Sufficient to buffer a burst of typed characters without loss. */

/* ── PUBLIC INTERFACE ───────────────────────────────────────────── */

void    keyboard_init(void);
/* Installs the keyboard IRQ handler on IRQ1 via irq_install().
   Unmasks IRQ1 at the master PIC.
   Initialises the ring buffer and modifier state. */

char    keyboard_getchar(void);
/* Returns the next character from the ring buffer.
   Returns 0 if the buffer is empty — non-blocking.
   Caller should poll until non-zero or use keyboard_haschar(). */

uint8_t keyboard_haschar(void);
/* Returns 1 if at least one character is waiting in the buffer.
   Returns 0 if the buffer is empty.
   Use in a spin loop: while (!keyboard_haschar()) {} */

uint8_t keyboard_ctrl(void);
/* Returns 1 if Ctrl is currently held, 0 otherwise.
   Used by the shell editor for Ctrl+S and Ctrl+X keybinds. */

uint8_t keyboard_alt(void);
/* Returns 1 if Left Alt is currently held, 0 otherwise.
   Used by the WM for Alt+Tab window cycling. */

/* Edge flags set by the keyboard IRQ on Up/Down arrow press.
   Read and clear these to scroll without consuming ring-buffer chars. */
extern volatile uint8_t kb_scroll_up;
extern volatile uint8_t kb_scroll_dn;
#endif
