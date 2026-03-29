
/* IronKernel — speaker.c
   PC speaker driven by PIT channel 2.
   Port 0x42 = PIT ch2 data, 0x43 = PIT command, 0x61 = speaker gate. */

#include "speaker.h"
#include "pit.h"
#include "../kernel/types.h"

/* ── I/O helpers ─────────────────────────────────────────────────── */

static inline void spk_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));
}
static inline uint8_t spk_inb(uint16_t port) {
    uint8_t r;
    __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port));
    return r;
}

#define PIT_CH2_DATA  0x42
#define PIT_CMD       0x43
#define SPK_PORT      0x61
#define PIT_BASE      1193180u   /* PIT input frequency Hz */

/* ── speaker_on ──────────────────────────────────────────────────── */

void speaker_on(uint32_t freq)
{
    if (!freq) { speaker_off(); return; }

    uint32_t div = PIT_BASE / freq;

    /* Program PIT channel 2: mode 3 (square wave), lo/hi byte */
    spk_outb(PIT_CMD, 0xB6);
    spk_outb(PIT_CH2_DATA, (uint8_t)(div & 0xFF));
    spk_outb(PIT_CH2_DATA, (uint8_t)(div >> 8));

    /* Enable speaker: set bits 0 (gate ch2) and 1 (speaker output) */
    spk_outb(SPK_PORT, spk_inb(SPK_PORT) | 0x03);
}

/* ── speaker_off ─────────────────────────────────────────────────── */

void speaker_off(void)
{
    spk_outb(SPK_PORT, spk_inb(SPK_PORT) & ~0x03u);
}

/* ── speaker_beep ────────────────────────────────────────────────── */

void speaker_beep(uint32_t freq, uint32_t ms)
{
    if (!ms) return;
    speaker_on(freq);
    /* 100 Hz PIT → 1 tick = 10 ms */
    uint64_t ticks = ms / 10;
    if (!ticks) ticks = 1;
    uint64_t end = pit_get_ticks() + ticks;
    while (pit_get_ticks() < end)
        __asm__ volatile("pause");
    speaker_off();
}

/* ── Note table helpers ──────────────────────────────────────────── */

typedef struct { uint32_t freq; uint32_t ms; } note_t;

static void play_notes(const note_t *notes, int count)
{
    for (int i = 0; i < count; i++) {
        if (notes[i].freq)
            speaker_beep(notes[i].freq, notes[i].ms);
        else {
            /* rest: silence for the given duration */
            uint64_t ticks = notes[i].ms / 10;
            if (!ticks) ticks = 1;
            uint64_t end = pit_get_ticks() + ticks;
            while (pit_get_ticks() < end)
                __asm__ volatile("pause");
        }
    }
}

/* ── Startup chime ───────────────────────────────────────────────── */
/*  C major arpeggio ascending → brief pause → resolve to high E.
    Sounds triumphant and clean on a square-wave speaker.           */

void speaker_startup_chime(void)
{
    static const note_t chime[] = {
        {523,  70},   /* C5  */
        {659,  70},   /* E5  */
        {784,  70},   /* G5  */
        {1047, 70},   /* C6  */
        {0,    45},   /* rest */
        {784,  60},   /* G5  */
        {1047, 60},   /* C6  */
        {1319, 310},  /* E6  — high resolution, held */
        {0,    60},   /* tail silence */
    };
    play_notes(chime, 9);
}

/* ── Panic sound ─────────────────────────────────────────────────── */
/*  Descending minor arpeggio ending on a low drone.
    Played after the panic screen is drawn; interrupts are briefly
    re-enabled inside panic_ex so pit_get_ticks() can advance.     */

void speaker_panic_sound(void)
{
    static const note_t sad[] = {
        {440, 140},   /* A4 — start */
        {349, 140},   /* F4 — minor third down */
        {294, 140},   /* D4 */
        {0,   70},    /* rest */
        {220, 480},   /* A3 — low ominous drone */
        {0,   60},
    };
    play_notes(sad, 6);
}
