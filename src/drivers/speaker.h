#ifndef SPEAKER_H
#define SPEAKER_H
#include "../kernel/types.h"

/* ── PC SPEAKER via PIT CHANNEL 2 ───────────────────────────────────
   PIT channel 2 (port 0x42) drives the PC speaker through port 0x61.
   Square wave output — no sound card required.
   ─────────────────────────────────────────────────────────────── */

/* Enable speaker at the given frequency (Hz). */
void speaker_on(uint32_t freq);

/* Silence the speaker immediately. */
void speaker_off(void);

/* Play a blocking tone: speaker_on → wait ms → speaker_off.
   Requires interrupts enabled (uses PIT ticks for timing). */
void speaker_beep(uint32_t freq, uint32_t ms);

/* Play the kernel startup chime (GUI desktop only). */
void speaker_startup_chime(void);

/* Play the kernel panic sound (uses busy-wait, IRQs may be off). */
void speaker_panic_sound(void);

#endif
