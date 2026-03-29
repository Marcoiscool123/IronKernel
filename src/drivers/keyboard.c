#include "keyboard.h"
#include "vga.h"
#include "pit.h"
#include "../kernel/types.h"

/* ── PORT I/O ───────────────────────────────────────────────────── */

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %1, %0" : : "dN"(port), "a"(val));
}

/* ── SCANCODE → ASCII TABLE (Set 1, unshifted) ──────────────────────
   Index = scancode make code (0x00–0x58).
   Value = ASCII character, or 0 for non-printable keys.
   This is IBM PC Scancode Set 1 — the default used by all
   PC-compatible BIOS implementations and QEMU.
   ─────────────────────────────────────────────────────────────── */

static const char scancode_table[] = {
/*00*/  0,
/*01*/  0,       /* Escape */
/*02*/  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
/*0C*/  '-', '=',
/*0E*/  '\b',    /* Backspace */
/*0F*/  '\t',    /* Tab */
/*10*/  'q','w','e','r','t','y','u','i','o','p','[',']',
/*1C*/  '\n',    /* Enter */
/*1D*/  0,       /* Left Ctrl */
/*1E*/  'a','s','d','f','g','h','j','k','l',';','\'','`',
/*2A*/  0,       /* Left Shift */
/*2B*/  '\\',
/*2C*/  'z','x','c','v','b','n','m',',','.','/',
/*36*/  0,       /* Right Shift */
/*37*/  '*',     /* Keypad * */
/*38*/  0,       /* Left Alt */
/*39*/  ' ',     /* Space */
/*3A*/  0,       /* CapsLock */
/*3B*/  0,0,0,0,0,0,0,0,0,0, /* F1–F10 */
/*45*/  0,       /* NumLock */
/*46*/  0,       /* ScrollLock */
/*47*/  0,0,0,   /* Keypad 7,8,9 */
/*4A*/  '-',     /* Keypad - */
/*4B*/  0,0,0,   /* Keypad 4,5,6 */
/*4E*/  '+',     /* Keypad + */
/*4F*/  0,0,0,0, /* Keypad 1,2,3,0 */
/*53*/  0,       /* Keypad . */
/*54*/  0,0,0,   /* unused */
/*57*/  0,0      /* F11, F12 */
};

/* ── SCANCODE → ASCII TABLE (Set 1, shifted) ───────────────────── */

static const char scancode_table_shift[] = {
/*00*/  0,
/*01*/  0,
/*02*/  '!','@','#','$','%','^','&','*','(',')','_','+',
/*0E*/  '\b',
/*0F*/  '\t',
/*10*/  'Q','W','E','R','T','Y','U','I','O','P','{','}',
/*1C*/  '\n',
/*1D*/  0,
/*1E*/  'A','S','D','F','G','H','J','K','L',':','"','~',
/*2A*/  0,
/*2B*/  '|',
/*2C*/  'Z','X','C','V','B','N','M','<','>','?',
/*36*/  0,
/*37*/  '*',
/*38*/  0,
/*39*/  ' ',
/*3A*/  0,
/*3B*/  0,0,0,0,0,0,0,0,0,0,
/*45*/  0,
/*46*/  0,
/*47*/  0,0,0,'-',0,0,0,'+',0,0,0,0,
/*53*/  0,0,0,0,0,0
};

#define SCANCODE_TABLE_SIZE  ((uint8_t)(sizeof(scancode_table) / sizeof(scancode_table[0])))

/* ── RING BUFFER ────────────────────────────────────────────────── */

static volatile char    kb_buffer[KB_BUFFER_SIZE];
static volatile uint8_t kb_head = 0;
/* Head: index of the next byte to READ. Advances on getchar(). */

static volatile uint8_t kb_tail = 0;
/* Tail: index of the next byte to WRITE. Advances on keypress. */

/* volatile: both head and tail are modified in the IRQ handler
   (interrupt context) and read in the main kernel loop.
   Without volatile, the compiler may cache them in registers
   and never see updates made by the interrupt handler. */

/* ── MODIFIER STATE ─────────────────────────────────────────────── */

static uint8_t shift_held = 0;
/* 1 if either Shift key is currently pressed, 0 otherwise.
   Updated on make (press) and break (release) of shift scancodes. */

static uint8_t caps_lock  = 0;
static uint8_t ext_mode   = 0;
static uint8_t ctrl_pressed = 0;
static uint8_t alt_held     = 0;

/* Edge flags for scroll keys — set in IRQ, read+cleared by WM.
   Separate from the ring buffer so WM can scroll ELF windows
   without consuming chars that ELF's SYS_READ is waiting for. */
volatile uint8_t kb_scroll_up = 0;
volatile uint8_t kb_scroll_dn = 0;
/* Tracks whether Left or Right Ctrl is currently held down.
   Set on scancode 0x1D (press), cleared on 0x9D (release).
   Right Ctrl uses extended prefix 0xE0 + 0x1D. */
/* 1 after receiving 0xE0 prefix — next scancode is an extended key.
   Extended keys: arrow keys, PgUp, PgDn, Home, End, Insert, Delete. */
/* 1 if CapsLock is active, 0 otherwise.
   Toggled on each CapsLock key press (make code only). */

/* ── IRQ1 HANDLER ───────────────────────────────────────────────── */

static void keyboard_callback(void)
{
    uint8_t scancode = inb(KB_DATA_PORT);
    /* Read the scancode from the keyboard controller.
       This MUST be done in the IRQ handler — if we don't read it,
       the 8042 will not generate another IRQ for the next key. */

    /* Handle 0xE0 extended scancode prefix. */
    if (scancode == 0xE0) {
        ext_mode = 1;
        return;
        /* Next scancode will be the actual extended key. */
    }

    /* Ctrl press (0x1D) and release (0x9D) — both normal and extended. */
    if (scancode == 0x1D) { ctrl_pressed = 1; return; }
    if (scancode == 0x9D) { ctrl_pressed = 0; return; }

    if (ext_mode) {
        ext_mode = 0;
        if (scancode & KB_RELEASE_BIT) return;
        /* Extended key release — ignore. */

        /* Extended make codes we care about: */
        uint8_t next_tail = (kb_tail + 1) & (KB_BUFFER_SIZE - 1);
        if (next_tail == kb_head) return;
        if (scancode == 0x48) {
            kb_buffer[kb_tail] = KEY_UP;
            kb_tail = next_tail;
            kb_scroll_up = 1;   /* WM edge flag */
        } else if (scancode == 0x50) {
            kb_buffer[kb_tail] = KEY_DOWN;
            kb_tail = next_tail;
            kb_scroll_dn = 1;   /* WM edge flag */
        } else if (scancode == 0x4B) {
            kb_buffer[kb_tail] = KEY_LEFT;
            kb_tail = next_tail;
        } else if (scancode == 0x4D) {
            kb_buffer[kb_tail] = KEY_RIGHT;
            kb_tail = next_tail;
        }
        return;
    }

    if (scancode & KB_RELEASE_BIT) {
        /* Break code — key was released. */
        uint8_t key = scancode & ~KB_RELEASE_BIT;
        if (key == KB_LSHIFT || key == KB_RSHIFT) shift_held = 0;
        if (key == 0x38) alt_held = 0;   /* Left Alt release */
        return;
        /* Ignore all other break codes — we only act on presses. */
    }

    /* Make code — key was pressed. */
    if (scancode == KB_LSHIFT || scancode == KB_RSHIFT) {
        shift_held = 1;
        return;
    }

    if (scancode == 0x38) { alt_held = 1; return; }  /* Left Alt press */

    if (scancode == KB_CAPS) {
        caps_lock ^= 1;
        /* Toggle CapsLock on each press. */
        return;
    }

    if (scancode >= SCANCODE_TABLE_SIZE) return;
    /* Unknown or extended scancode — ignore.
       Extended scancodes start with 0xE0 prefix (e.g. arrow keys)
       and require multi-byte handling. Out of scope for this Node. */

    char c = 0;

    if (shift_held) {
        c = scancode_table_shift[scancode];
    } else {
        c = scancode_table[scancode];
        /* Apply CapsLock: if caps is active and the key is a
           lowercase letter, convert to uppercase. */
        if (caps_lock && c >= 'a' && c <= 'z') {
            c -= 32;
            /* ASCII lowercase to uppercase: subtract 32. */
        }
    }

    /* Encode Ctrl+letter as ASCII control character (1-26). */
    if (ctrl_pressed && c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 1);
    else if (ctrl_pressed && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 1);

    if (!c) return;
    /* Non-printable key (Ctrl, Alt, F-key etc.) — discard. */

    /* Write character into the ring buffer. */
    uint8_t next_tail = (kb_tail + 1) & (KB_BUFFER_SIZE - 1);
    /* Advance tail with power-of-2 wrap.
       & (SIZE-1) is equivalent to % SIZE but branch-free. */

    if (next_tail == kb_head) return;
    /* Buffer full — drop the character.
       Better to lose one key than to corrupt the buffer state. */

    kb_buffer[kb_tail] = c;
    kb_tail = next_tail;
}

/* ── PUBLIC FUNCTIONS ───────────────────────────────────────────── */

void keyboard_init(void)
{
    shift_held = 0;
    caps_lock  = 0;
    kb_head    = 0;
    kb_tail    = 0;

    /* Flush any stale data left in the 8042 output buffer by GRUB or BIOS.
       If OBF (bit 0 of status port 0x64) is set, a byte is waiting at 0x60.
       Reading it clears OBF. Without this, a stale shift-key scancode fed to
       keyboard_callback on the first IRQ permanently sticks shift_held = 1. */
    while (inb(0x64) & 0x01)
        inb(0x60);

    irq_install(1, keyboard_callback);
    /* Register our handler in the PIT's IRQ handler table for IRQ1.
       irq_install is defined in pit.c and exposed via pit.h. */

    /* Unmask IRQ1 at the master PIC. */
    uint8_t mask = inb(0x21);
    mask &= ~(1 << 1);
    /* Clear bit 1 = unmask IRQ1 (keyboard).
       All other IRQ masks are preserved. */
    outb(0x21, mask);

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("[KB] KEYBOARD ONLINE\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("     IRQ1     : UNMASKED\n");
    vga_print("     BUFFER   : 256 bytes\n");
    vga_print("     ENCODING : PS/2 Set 1 -> ASCII\n");
}

char keyboard_getchar(void)
{
    if (kb_head == kb_tail) return 0;
    /* Buffer empty — return null. Caller must check. */

    char c = kb_buffer[kb_head];
    kb_head = (kb_head + 1) & (KB_BUFFER_SIZE - 1);
    /* Advance head with wrap. */
    return c;
}

uint8_t keyboard_haschar(void)
{
    return kb_head != kb_tail;
    /* Non-zero if head and tail differ — at least one char waiting. */
}

uint8_t keyboard_ctrl(void)
{
    return ctrl_pressed;
}

uint8_t keyboard_alt(void)
{
    return alt_held;
}
