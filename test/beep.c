/* BEEP.ELF — PC speaker demo.
   Works in both text mode and GUI mode (pure hardware, no GFX needed).

   Usage:
     exec BEEP.ELF          — plays the built-in demo melody
     exec BEEP.ELF 440 500  — plays 440 Hz for 500 ms            */

#include <ironkernel.h>

/* ── tiny helpers ──────────────────────────────────────────────── */

static uint32_t parse_u32(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static void skip_spaces(const char **s)
{
    while (**s == ' ') (*s)++;
}

/* ── note table ────────────────────────────────────────────────── */

typedef struct { uint32_t freq; uint32_t ms; } note_t;

static void play(const note_t *n, int count)
{
    for (int i = 0; i < count; i++) {
        ik_beep(n[i].freq, n[i].ms);
        /* small gap between notes so they're distinct */
        if (n[i].freq && i + 1 < count && n[i+1].freq)
            ik_beep(0, 30);
    }
}

/* ── entry ─────────────────────────────────────────────────────── */

int main(void)
{
    char arg[32];
    ik_get_arg(arg, sizeof(arg));

    if (arg[0]) {
        /* Custom freq + ms from argument: "440 500" */
        const char *p = arg;
        skip_spaces(&p);
        uint32_t freq = parse_u32(p);
        while (*p && *p != ' ') p++;
        skip_spaces(&p);
        uint32_t ms = *p ? parse_u32(p) : 300;
        if (!ms) ms = 300;

        ik_write("\n  Playing: ");
        /* print freq */
        char buf[12]; int i = 10; buf[11] = 0;
        uint32_t v = freq;
        if (!v) { buf[i--] = '0'; } else { while (v) { buf[i--]='0'+(v%10); v/=10; } }
        ik_write(&buf[i+1]);
        ik_write(" Hz\n");

        ik_beep(freq, ms);

    } else {
        /* Demo melody: Super Mario-ish fanfare */
        ik_write("\n  BEEP.ELF — PC Speaker Demo\n");
        ik_write("  Playing demo melody...\n");

        static const note_t melody[] = {
            {660, 100}, {660, 100}, {0,  50}, {660, 100},
            {0,   50},  {510, 100}, {660, 100}, {0, 50},
            {770, 150}, {0,  200},  {380, 150},
        };
        play(melody, 11);

        ik_write("  Done.\n");
        ik_write("  Tip: exec BEEP.ELF 440 500\n");
    }

    return 0;
}
