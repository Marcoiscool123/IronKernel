/* IronKernel GUI Toolkit — ikgfx.h
   Header-only pixel-mode GUI library for IronKernel ELF programs.

   Usage:
     #include <ikgfx.h>

     int main(void) {
         ik_gfx_init();          // enter pixel mode
         ik_gfx_grad(0,0,w,h, 0x0A1422, 0x060C18);  // background
         ik_button(10,10,80,30,"Click me");
         ik_gfx_flush();         // tell WM to redraw
         uint32_t click = ik_get_click();
         if (ik_hit(click, 10,10,80,30)) { ... }
     }
*/

#ifndef IKGFX_H
#define IKGFX_H

#include <stdint.h>

/* ── Syscall numbers ─────────────────────────────────────────────── */
#define _IKGFX_INIT   24   /* SYS_WIN_GFX_INIT  */
#define _IKGFX_RECT   25   /* SYS_WIN_GFX_RECT  */
#define _IKGFX_STR    26   /* SYS_WIN_GFX_STR   */
#define _IKGFX_GRAD   27   /* SYS_WIN_GFX_GRAD  */
#define _IKGFX_FLUSH  28   /* SYS_WIN_GFX_FLUSH */
#define _IKGFX_CLICK  29   /* SYS_GET_CLICK     */
#define _IKGFX_RDKEY  30   /* SYS_READ_RAW      */

/* ── Raw key codes (returned by ik_read_key) ──────────────────────── */
#define IK_KEY_UP    0x01
#define IK_KEY_DOWN  0x02
#define IK_KEY_LEFT  0x80
#define IK_KEY_RIGHT 0x81
#define IK_KEY_BACK  0x08
#define IK_KEY_ENTER 0x0A

/* ── Color palette ── */
#define IK_BG      0x080C16u   /* window background          */
#define IK_TEXT    0xC8D4E8u   /* normal text                */
#define IK_ACCENT  0x70A8F0u   /* highlight / label accent   */
#define IK_WHITE   0xFFFFFFu
#define IK_BLACK   0x020406u
#define IK_BTNHI   0x5888D8u   /* button upper highlight     */
#define IK_BTNLO   0x1840A0u   /* button lower shade         */
#define IK_OPHI    0x7898E8u   /* operator button hi         */
#define IK_OPLO    0x2840A0u   /* operator button lo         */
#define IK_EQHI    0x40C860u   /* equals button hi           */
#define IK_EQLO    0x108030u   /* equals button lo           */
#define IK_DISPBG  0x040810u   /* display/textbox background */

/* ── Low-level syscall wrappers ────────────────────────────────── */

static inline void ik_gfx_init(void)
{
    __asm__ volatile(
        "mov %0, %%rax\n\t"
        "int $0x80"
        :: "i"(_IKGFX_INIT) : "rax", "memory");
}

static inline void ik_gfx_rect(int x, int y, int w, int h, uint32_t color)
{
    uint64_t xy = ((uint64_t)(uint32_t)x << 16) | (uint16_t)(uint32_t)y;
    uint64_t wh = ((uint64_t)(uint32_t)w << 16) | (uint16_t)(uint32_t)h;
    __asm__ volatile(
        "mov %0, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "mov %2, %%rcx\n\t"
        "mov %3, %%rdx\n\t"
        "int $0x80"
        :: "i"(_IKGFX_RECT),
           "r"(xy), "r"(wh), "r"((uint64_t)color)
        : "rax","rbx","rcx","rdx");
}

static inline void ik_gfx_str(int x, int y, const char *s, uint32_t color)
{
    uint64_t xy = ((uint64_t)(uint32_t)x << 16) | (uint16_t)(uint32_t)y;
    __asm__ volatile(
        "mov %0, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "mov %2, %%rcx\n\t"
        "mov %3, %%rdx\n\t"
        "int $0x80"
        :: "i"(_IKGFX_STR),
           "r"(xy), "r"((uint64_t)(uintptr_t)s), "r"((uint64_t)color)
        : "rax","rbx","rcx","rdx","memory");
}

static inline void ik_gfx_grad(int x, int y, int w, int h,
                                uint32_t c1, uint32_t c2)
{
    uint64_t xy = ((uint64_t)(uint32_t)x << 16) | (uint16_t)(uint32_t)y;
    uint64_t wh = ((uint64_t)(uint32_t)w << 16) | (uint16_t)(uint32_t)h;
    uint32_t cols[2];
    cols[0] = c1; cols[1] = c2;
    __asm__ volatile(
        "mov %0, %%rax\n\t"
        "mov %1, %%rbx\n\t"
        "mov %2, %%rcx\n\t"
        "mov %3, %%rdx\n\t"
        "int $0x80"
        :: "i"(_IKGFX_GRAD),
           "r"(xy), "r"(wh), "r"((uint64_t)(uintptr_t)cols)
        : "rax","rbx","rcx","rdx","memory");
}

static inline void ik_gfx_flush(void)
{
    __asm__ volatile(
        "mov %0, %%rax\n\t"
        "int $0x80"
        :: "i"(_IKGFX_FLUSH) : "rax");
}

/* Read one raw keystroke; blocks until a key is pressed.
   Returns the raw character (see IK_KEY_* constants above). */
static inline int ik_read_key(void)
{
    uint64_t ret;
    __asm__ volatile(
        "mov %1, %%rax\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "i"(_IKGFX_RDKEY)
        : "rax");
    return (int)(uint8_t)ret;
}

static inline uint32_t ik_get_click(void)
{
    uint64_t ret;
    __asm__ volatile(
        "mov %1, %%rax\n\t"
        "int $0x80\n\t"
        "mov %%rax, %0"
        : "=r"(ret)
        : "i"(_IKGFX_CLICK)
        : "rax");
    return (uint32_t)ret;
}

/* ── Click decode helpers ─────────────────────────────────────── */
static inline int ik_click_x(uint32_t click) { return (int)(click >> 16); }
static inline int ik_click_y(uint32_t click) { return (int)(click & 0xFFFFu); }
static inline int ik_hit(uint32_t click, int x, int y, int w, int h)
{
    int cx = ik_click_x(click), cy = ik_click_y(click);
    return cx >= x && cx < x + w && cy >= y && cy < y + h;
}

/* ── Widget: ik_button ──────────────────────────────────────────
   Draws a glassy Aero-style button.  hi/lo are the gradient colors;
   pass 0 for defaults (blue style).                               */
static inline void ik_button_ex(int x, int y, int w, int h,
                                 const char *label,
                                 uint32_t hi, uint32_t lo)
{
    if (!hi) hi = IK_BTNHI;
    if (!lo) lo = IK_BTNLO;
    int half = h / 2;
    /* Upper glass */
    ik_gfx_grad(x+1, y+1,    w-2, half-1,   hi,             lo);
    /* Lower reflection */
    ik_gfx_grad(x+1, y+half, w-2, h-half-1, (lo>>1)&0x7F7F7Fu, lo);
    /* Top gloss line */
    ik_gfx_rect(x+1, y+1, w-2, 1, (hi | 0x404040u) & 0xFFFFFFu);
    /* Border */
    ik_gfx_rect(x,     y,     w, 1, hi);          /* top    */
    ik_gfx_rect(x,     y,     1, h, hi);          /* left   */
    ik_gfx_rect(x,     y+h-1, w, 1, 0x101828u);  /* bottom */
    ik_gfx_rect(x+w-1, y,     1, h, 0x101828u);  /* right  */
    /* Centered label */
    if (label && *label) {
        int llen = 0;
        const char *p = label; while (*p++) llen++;
        int lx = x + (w - llen * 8) / 2;
        int ly = y + (h - 8)       / 2;
        if (lx < x + 2) lx = x + 2;
        ik_gfx_str(lx+1, ly+1, label, 0x00101Cu);  /* shadow */
        ik_gfx_str(lx,   ly,   label, IK_WHITE);
    }
}

static inline void ik_button(int x, int y, int w, int h, const char *label)
{
    ik_button_ex(x, y, w, h, label, IK_BTNHI, IK_BTNLO);
}

/* ── Widget: ik_label ── */
static inline void ik_label(int x, int y, const char *text, uint32_t color)
{
    ik_gfx_str(x, y, text, color);
}

/* ── Widget: ik_textbox ── */
static inline void ik_textbox(int x, int y, int w, int h)
{
    ik_gfx_rect(x+1, y+1, w-2, h-2, IK_DISPBG);         /* interior */
    ik_gfx_rect(x,     y,     w, 1, 0x0C1828u);          /* top dark  */
    ik_gfx_rect(x,     y,     1, h, 0x0C1828u);          /* left dark */
    ik_gfx_rect(x,     y+h-1, w, 1, 0x304858u);          /* bot light */
    ik_gfx_rect(x+w-1, y,     1, h, 0x304858u);          /* rgt light */
}

#endif /* IKGFX_H */
