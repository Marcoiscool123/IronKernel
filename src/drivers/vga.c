#include "vga.h"
#include "../kernel/scroll.h"
#include "../drivers/serial.h"
#include "../drivers/pit.h"

/* ── FRAMEBUFFER POINTER & DIMENSIONS ───────────────────────────── */

static uint8_t  *GFX_MEM    = (uint8_t*)0xA0000; /* updated by vga_set_fb  */
static uint64_t  g_fb_addr  = 0;                 /* VBE LFB phys addr      */
static int       g_fb_w     = 320;               /* current fb width       */
static int       g_fb_h     = 200;               /* current fb height      */
static int       g_fb_pitch = 320;               /* bytes per scanline     */

/* ── I/O PORT HELPERS ────────────────────────────────────────────── */

static inline void gfx_outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %1, %0" : : "dN"(port), "a"(val));
}
static inline void gfx_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %1, %0" : : "dN"(port), "a"(val));
}
static inline uint8_t gfx_inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}
static inline void gfx_outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %1, %0" : : "dN"(port), "a"(val));
}
static inline uint32_t gfx_inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

/* ── VBE SETUP ───────────────────────────────────────────────────── */

/* VGA 16-colour to 32bpp palette table. */
static const uint32_t pal32[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static void vbe_set_mode(uint16_t w, uint16_t h, uint16_t bpp)
{
    gfx_outw(0x01CE, 4); gfx_outw(0x01CF, 0);     /* disable VBE     */
    gfx_outw(0x01CE, 1); gfx_outw(0x01CF, w);     /* XRES            */
    gfx_outw(0x01CE, 2); gfx_outw(0x01CF, h);     /* YRES            */
    gfx_outw(0x01CE, 3); gfx_outw(0x01CF, bpp);   /* BPP             */
    gfx_outw(0x01CE, 4); gfx_outw(0x01CF, 1);     /* enable VBE      */
}

/* ── PIXEL TERMINAL STATE ───────────────────────────────────────── */

typedef struct { char ch; uint8_t fg, bg; } pt_cell_t;

static pt_cell_t pt_grid[PT_ROWS][PT_COLS]; /* live character grid */

/* ── SCROLLBACK RING ────────────────────────────────────────────── */

#define SB_LINES 200
#define SB_STEP   12

static pt_cell_t sb_buf[SB_LINES][PT_COLS];
static int sb_head  = 0;   /* next write slot */
static int sb_count = 0;   /* lines stored    */
static int view_off = 0;   /* 0=live; N=N rows back */

/* ── CURSOR & COLOUR ────────────────────────────────────────────── */

static int      pt_row  = 0, pt_col  = 0;
static uint8_t  pt_fg   = 7, pt_bg   = 0;
static uint32_t pt_fg32 = 0xAAAAAA;  /* pal32[7] */
static uint32_t pt_bg32 = 0x000000;  /* pal32[0] */

/* ── INTERNAL HELPERS ───────────────────────────────────────────── */

static void pt_draw_cell(int px, int py, pt_cell_t cell)
{
    vga_rect(px, py, CHAR_W, LINE_H, pal32[cell.bg & 0x0F]);
    if ((unsigned char)cell.ch >= 0x20)
        vga_blit_char(px, py, cell.ch, pal32[cell.fg & 0x0F]);
}

static void render_screen(void)
{
    /* Don't call vga_gfx_clear — each cell paints its own background,
       so clearing first is 307K wasted MMIO writes.  Only clear the
       3 pixel rows below the last text row that cells don't cover. */
    {
        uint64_t zero = 0;
        int term_w8 = (PT_COLS * CHAR_W) >> 1;
        for (int y = PT_ROWS * LINE_H; y < g_fb_h; y++) {
            uint64_t *row = (uint64_t*)(GFX_MEM + (uint32_t)(y * g_fb_pitch));
            for (int w = 0; w < term_w8; w++) row[w] = zero;
        }
    }
    for (int r = 0; r < PT_ROWS; r++) {
        int py = r * LINE_H;
        if (r < view_off) {
            int idx = ((sb_head - view_off + r) % SB_LINES + SB_LINES) % SB_LINES;
            for (int c = 0; c < PT_COLS; c++)
                pt_draw_cell(c * CHAR_W, py, sb_buf[idx][c]);
        } else {
            int gr = r - view_off;
            for (int c = 0; c < PT_COLS; c++)
                pt_draw_cell(c * CHAR_W, py, pt_grid[gr][c]);
        }
    }
}

static void pt_scroll(void)
{
    /* Save departing top line to ring */
    for (int c = 0; c < PT_COLS; c++)
        sb_buf[sb_head][c] = pt_grid[0][c];
    sb_head = (sb_head + 1) % SB_LINES;
    if (sb_count < SB_LINES) sb_count++;

    /* Keep user's view anchored */
    if (view_off > 0 && view_off < sb_count)
        view_off++;

    /* Shift grid rows up */
    for (int r = 0; r < PT_ROWS - 1; r++)
        for (int c = 0; c < PT_COLS; c++)
            pt_grid[r][c] = pt_grid[r + 1][c];
    for (int c = 0; c < PT_COLS; c++)
        pt_grid[PT_ROWS - 1][c] = (pt_cell_t){' ', pt_fg, pt_bg};

    /* Shift pixels (live view only) — copy 8 bytes per iteration.
       term_w = PT_COLS*CHAR_W = 100*8 = 800, always a multiple of 8.
       g_fb_pitch = 3200 bytes, row starts are 8-byte aligned. */
    if (view_off == 0) {
        int split    = (PT_ROWS - 1) * LINE_H;
        int term_w8  = (PT_COLS * CHAR_W) >> 1;   /* 400 uint64 words per row (32bpp) */
        for (int y = 0; y < split; y++) {
            uint64_t *dst = (uint64_t*)(GFX_MEM + (uint32_t)(y          * g_fb_pitch));
            uint64_t *src = (uint64_t*)(GFX_MEM + (uint32_t)((y + LINE_H) * g_fb_pitch));
            for (int w = 0; w < term_w8; w++) dst[w] = src[w];
        }
        uint64_t fill = ((uint64_t)pt_bg32 << 32) | (uint64_t)pt_bg32;
        for (int y = split; y < PT_ROWS * LINE_H; y++) {
            uint64_t *dst = (uint64_t*)(GFX_MEM + (uint32_t)(y * g_fb_pitch));
            for (int w = 0; w < term_w8; w++) dst[w] = fill;
        }
    }
    pt_row = PT_ROWS - 1;
}

/* ── PUBLIC FUNCTIONS ───────────────────────────────────────────── */

void vga_set_fb(uint64_t addr, uint32_t pitch)
{
    g_fb_addr  = addr;
    g_fb_pitch = (int)pitch;
}

void vga_scroll_init(void)
{
    sb_head = 0; sb_count = 0; view_off = 0;
}

int vga_scroll_active(void)
{
    return view_off != 0;
}

void vga_init(void)
{
    if (!g_fb_addr) {
        /* GRUB didn't hand us a framebuffer tag.
           Query PCI config space for Bochs VGA BAR0 (bus 0, dev 2, fn 0).
           PCI address format: 0x80000000 | bus<<16 | dev<<11 | fn<<8 | reg */
        uint32_t pci_addr = 0x80000000u | (2u << 11) | 0x10u; /* BAR0 at offset 0x10 */
        gfx_outl(0xCF8, pci_addr);
        uint32_t bar0 = gfx_inl(0xCFC) & 0xFFFFFFF0u; /* mask type bits */
        if (bar0 >= 0xC0000000u)
            g_fb_addr = bar0;
    }

    if (g_fb_addr) {
        /* VBE mode: program 800x600x32 into Bochs VBE controller then map LFB.
           Only program the hardware once — reprogramming an already-active VBE
           mode resets the controller and can crash or corrupt the display. */
        static int vbe_initialized = 0;
        if (!vbe_initialized) {
            vbe_set_mode(800, 600, 32);
            GFX_MEM    = (uint8_t*)(uintptr_t)g_fb_addr;
            g_fb_w     = 800;
            g_fb_h     = 600;
            g_fb_pitch = 800 * 4;   /* 3200 bytes per scanline */
            vbe_initialized = 1;
        }
        uint32_t *fb32 = (uint32_t*)(uintptr_t)g_fb_addr;
        int pixels = 800 * 600;
        for (int i = 0; i < pixels; i++) fb32[i] = 0;
    } else {
        /* Last-resort fallback: legacy Mode 13h 320x200 at 0xA0000 */
        vga_set_mode13h();
    }
    pt_row = 0; pt_col = 0;
    pt_fg  = 7; pt_bg  = 0;
    view_off = 0; sb_head = 0; sb_count = 0;
    for (int r = 0; r < PT_ROWS; r++)
        for (int c = 0; c < PT_COLS; c++)
            pt_grid[r][c] = (pt_cell_t){' ', 7, 0};
}

void vga_set_color(vga_color_t fg, vga_color_t bg)
{
    if (vga_color_hook) { vga_color_hook((uint8_t)fg, (uint8_t)bg); return; }
    pt_fg = (uint8_t)(fg & 0x0F);
    pt_bg = (uint8_t)(bg & 0x0F);
    pt_fg32 = pal32[pt_fg];
    pt_bg32 = pal32[pt_bg];
}

void vga_putchar(char c)
{
    if (c == '\n') {
        pt_col = 0;
        pt_row++;
        if (pt_row >= PT_ROWS) pt_scroll();
        return;
    }
    if (pt_row < PT_ROWS && pt_col < PT_COLS)
        pt_grid[pt_row][pt_col] = (pt_cell_t){c, pt_fg, pt_bg};
    if (view_off == 0)
        pt_draw_cell(pt_col * CHAR_W, pt_row * LINE_H,
                     (pt_cell_t){c, pt_fg, pt_bg});
    pt_col++;
    if (pt_col >= PT_COLS) {
        pt_col = 0;
        pt_row++;
        if (pt_row >= PT_ROWS) pt_scroll();
    }
}

/* ── Output hooks (NULL by default; set by WM to redirect output) ── */
void (*vga_print_hook)(const char *s)          = (void*)0;
void (*vga_color_hook)(uint8_t fg, uint8_t bg) = (void*)0;

void vga_print(const char* str)
{
    if (vga_print_hook) { vga_print_hook(str); return; }
    while (*str) vga_putchar(*str++);
}

void vga_backspace(void)
{
    if (pt_col > 0) {
        pt_col--;
    } else if (pt_row > 0) {
        pt_row--;
        pt_col = PT_COLS - 1;
    } else {
        return;
    }
    pt_grid[pt_row][pt_col] = (pt_cell_t){' ', pt_fg, pt_bg};
    if (view_off == 0)
        vga_rect(pt_col * CHAR_W, pt_row * LINE_H, CHAR_W, LINE_H, pt_bg32);
}

void vga_set_cursor(void) { /* no hardware cursor in Mode 13h */ }

void vga_goto(uint8_t row, uint8_t col)
{
    pt_row = (row < PT_ROWS) ? (int)row : PT_ROWS - 1;
    pt_col = (col < PT_COLS) ? (int)col : PT_COLS - 1;
}

void vga_get_cursor(int *row, int *col) { *row = pt_row; *col = pt_col; }

void vga_write_at(uint8_t row, uint8_t col, const char* str,
                  vga_color_t fg, vga_color_t bg)
{
    if (row >= PT_ROWS) return;
    for (int c = (int)col; c < PT_COLS; c++) {
        char ch = *str ? *str++ : ' ';
        pt_cell_t cell = {ch, (uint8_t)fg, (uint8_t)bg};
        pt_grid[row][c] = cell;
        if (view_off == 0)
            pt_draw_cell(c * CHAR_W, (int)row * LINE_H, cell);
    }
}

void vga_view_up(void)
{
    if (sb_count == 0) return;
    int avail = sb_count - view_off;
    if (avail <= 0) return;
    view_off += (avail < SB_STEP) ? avail : SB_STEP;
    render_screen();
}

void vga_view_down(void)
{
    if (view_off == 0) return;
    view_off -= (view_off < SB_STEP) ? view_off : SB_STEP;
    render_screen();
}

void vga_view_reset(void)
{
    if (view_off == 0) return;
    view_off = 0;
    render_screen();
}

/* ── MODE 13h / GFX FUNCTIONS ───────────────────────────────────────
   Graphics functions use g_fb_w/g_fb_h/g_fb_pitch so they work in
   both VBE mode (640x480) and legacy Mode 13h (320x200, 0xA0000).
   ─────────────────────────────────────────────────────────────────── */

/* 8x8 IBM VGA font — indices 0..95 cover ASCII 0x20..0x7F */
static const uint8_t font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x20   */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 0x21 ! */
    {0x6C,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, /* 0x22 " */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* 0x23 # */
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, /* 0x24 $ */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* 0x25 % */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* 0x26 & */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* 0x27 ' */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* 0x28 ( */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* 0x29 ) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 0x2A * */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* 0x2B + */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* 0x2C , */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* 0x2D - */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* 0x2E . */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* 0x2F / */
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 0x30 0 */
    {0x30,0x70,0x30,0x30,0x30,0x30,0xFC,0x00}, /* 0x31 1 */
    {0x78,0xCC,0x0C,0x38,0x60,0xCC,0xFC,0x00}, /* 0x32 2 */
    {0x78,0xCC,0x0C,0x38,0x0C,0xCC,0x78,0x00}, /* 0x33 3 */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 0x34 4 */
    {0xFC,0xC0,0xF8,0x0C,0x0C,0xCC,0x78,0x00}, /* 0x35 5 */
    {0x38,0x60,0xC0,0xF8,0xCC,0xCC,0x78,0x00}, /* 0x36 6 */
    {0xFC,0xCC,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 0x37 7 */
    {0x78,0xCC,0xCC,0x78,0xCC,0xCC,0x78,0x00}, /* 0x38 8 */
    {0x78,0xCC,0xCC,0x7C,0x0C,0x18,0x70,0x00}, /* 0x39 9 */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* 0x3A : */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* 0x3B ; */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 0x3C < */
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, /* 0x3D = */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* 0x3E > */
    {0x78,0xCC,0x0C,0x18,0x18,0x00,0x18,0x00}, /* 0x3F ? */
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, /* 0x40 @ */
    {0x30,0x78,0xCC,0xCC,0xFC,0xCC,0xCC,0x00}, /* 0x41 A */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* 0x42 B */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* 0x43 C */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* 0x44 D */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* 0x45 E */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* 0x46 F */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* 0x47 G */
    {0xCC,0xCC,0xCC,0xFC,0xCC,0xCC,0xCC,0x00}, /* 0x48 H */
    {0x78,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* 0x49 I */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* 0x4A J */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* 0x4B K */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* 0x4C L */
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, /* 0x4D M */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* 0x4E N */
    {0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, /* 0x4F O */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* 0x50 P */
    {0x78,0xCC,0xCC,0xCC,0xDC,0x78,0x1C,0x00}, /* 0x51 Q */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* 0x52 R */
    {0x78,0xCC,0xE0,0x70,0x1C,0xCC,0x78,0x00}, /* 0x53 S */
    {0xFC,0xB4,0x30,0x30,0x30,0x30,0x78,0x00}, /* 0x54 T */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xFC,0x00}, /* 0x55 U */
    {0xCC,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* 0x56 V */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* 0x57 W */
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, /* 0x58 X */
    {0xCC,0xCC,0xCC,0x78,0x30,0x30,0x78,0x00}, /* 0x59 Y */
    {0xFC,0xC6,0x8C,0x18,0x32,0x66,0xFC,0x00}, /* 0x5A Z */
    {0x78,0x60,0x60,0x60,0x60,0x60,0x78,0x00}, /* 0x5B [ */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* 0x5C \ */
    {0x78,0x18,0x18,0x18,0x18,0x18,0x78,0x00}, /* 0x5D ] */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* 0x5E ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 0x5F _ */
    {0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00}, /* 0x60 ` */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* 0x61 a */
    {0xE0,0x60,0x60,0x7C,0x66,0x66,0xDC,0x00}, /* 0x62 b */
    {0x00,0x00,0x78,0xCC,0xC0,0xCC,0x78,0x00}, /* 0x63 c */
    {0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0x76,0x00}, /* 0x64 d */
    {0x00,0x00,0x78,0xCC,0xFC,0xC0,0x78,0x00}, /* 0x65 e */
    {0x38,0x6C,0x60,0xF0,0x60,0x60,0xF0,0x00}, /* 0x66 f */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 0x67 g */
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* 0x68 h */
    {0x30,0x00,0x70,0x30,0x30,0x30,0x78,0x00}, /* 0x69 i */
    {0x0C,0x00,0x0C,0x0C,0x0C,0xCC,0xCC,0x78}, /* 0x6A j */
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* 0x6B k */
    {0x70,0x30,0x30,0x30,0x30,0x30,0x78,0x00}, /* 0x6C l */
    {0x00,0x00,0xCC,0xFE,0xFE,0xD6,0xC6,0x00}, /* 0x6D m */
    {0x00,0x00,0xF8,0xCC,0xCC,0xCC,0xCC,0x00}, /* 0x6E n */
    {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0x00}, /* 0x6F o */
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* 0x70 p */
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* 0x71 q */
    {0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00}, /* 0x72 r */
    {0x00,0x00,0x7C,0xC0,0x78,0x0C,0xF8,0x00}, /* 0x73 s */
    {0x10,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, /* 0x74 t */
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* 0x75 u */
    {0x00,0x00,0xCC,0xCC,0xCC,0x78,0x30,0x00}, /* 0x76 v */
    {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00}, /* 0x77 w */
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* 0x78 x */
    {0x00,0x00,0xCC,0xCC,0xCC,0x7C,0x0C,0xF8}, /* 0x79 y */
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00}, /* 0x7A z */
    {0x1C,0x30,0x30,0xE0,0x30,0x30,0x1C,0x00}, /* 0x7B { */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 0x7C | */
    {0xE0,0x30,0x30,0x1C,0x30,0x30,0xE0,0x00}, /* 0x7D } */
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x7E ~ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x7F DEL */
};

void vga_set_mode13h(void)
{
    /* Miscellaneous Output register */
    gfx_outb(0x3C2, 0x63);

    /* Sequencer registers (index 0-4) */
    static const uint8_t seq_regs[5] = { 0x03, 0x01, 0x0F, 0x00, 0x0E };
    for (int i = 0; i < 5; i++) {
        gfx_outb(0x3C4, (uint8_t)i);
        gfx_outb(0x3C5, seq_regs[i]);
    }

    /* Unlock CRTC registers 0-7 (clear bit 7 of register 0x11) */
    gfx_outb(0x3D4, 0x11);
    uint8_t crtc11 = gfx_inb(0x3D5);
    gfx_outb(0x3D5, (uint8_t)(crtc11 & 0x7F));

    /* CRTC registers (index 0-24) */
    static const uint8_t crtc_regs[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
        0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF
    };
    for (int i = 0; i < 25; i++) {
        gfx_outb(0x3D4, (uint8_t)i);
        gfx_outb(0x3D5, crtc_regs[i]);
    }

    /* Graphics Controller registers (index 0-8) */
    static const uint8_t gc_regs[9] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF
    };
    for (int i = 0; i < 9; i++) {
        gfx_outb(0x3CE, (uint8_t)i);
        gfx_outb(0x3CF, gc_regs[i]);
    }

    /* Attribute Controller registers (index 0-20)
       Reset flip-flop by reading 0x3DA first, then write index+data to 0x3C0 */
    static const uint8_t ac_regs[21] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x41, 0x00, 0x0F, 0x00, 0x00
    };
    gfx_inb(0x3DA); /* reset flip-flop */
    for (int i = 0; i < 21; i++) {
        gfx_outb(0x3C0, (uint8_t)i);
        gfx_outb(0x3C0, ac_regs[i]);
    }
    gfx_outb(0x3C0, 0x20); /* enable video output */

    /* Switch back to Mode 13h context */
    gfx_outw(0x01CE, 4); gfx_outw(0x01CF, 0); /* disable VBE */
    GFX_MEM    = (uint8_t*)0xA0000;
    g_fb_w     = 320;
    g_fb_h     = 200;
    g_fb_pitch = 320;

    /* Clear framebuffer */
    for (int i = 0; i < 320 * 200; i++)
        GFX_MEM[i] = 0;
}

void vga_gfx_clear(uint32_t color)
{
    uint64_t fill = ((uint64_t)color << 32) | (uint64_t)color;
    int words = g_fb_w * g_fb_h / 2;
    uint64_t *p = (uint64_t*)GFX_MEM;
    for (int i = 0; i < words; i++) p[i] = fill;
}

void vga_redraw(void)
{
    render_screen();
}

void vga_pixel(int x, int y, uint32_t color)
{
    if (x < 0 || x >= g_fb_w || y < 0 || y >= g_fb_h) return;
    uint32_t *p = (uint32_t*)(GFX_MEM + (uint32_t)(y * g_fb_pitch) + (uint32_t)(x * 4));
    *p = color;
}

uint32_t vga_read_pixel(int x, int y)
{
    if (x < 0 || x >= g_fb_w || y < 0 || y >= g_fb_h) return 0;
    uint32_t *p = (uint32_t*)(GFX_MEM + (uint32_t)(y * g_fb_pitch) + (uint32_t)(x * 4));
    return *p;
}

/* ── Double-buffering for WM (flicker-free rendering) ────────────────
 * wm_back_buf is a full-frame off-screen buffer.
 * vga_begin_frame() redirects all pixel writes there.
 * vga_end_frame()   blits the result to the real screen atomically.
 * vga_blit_frame()  re-blits the existing back buffer (cursor erase).
 */
static uint8_t  wm_back_buf[800 * 600 * 4];
static uint8_t *real_gfx_mem = (uint8_t*)0;

void vga_blit_pixels(int dx, int dy, int w, int h,
                     const uint32_t *src, int src_stride)
{
    /* Clip to screen bounds */
    if (dx < 0) { src -= dx; w += dx; dx = 0; }
    if (dy < 0) { src -= (long)dy * src_stride; h += dy; dy = 0; }
    if (dx + w > g_fb_w) w = g_fb_w - dx;
    if (dy + h > g_fb_h) h = g_fb_h - dy;
    if (w <= 0 || h <= 0) return;

    int pitch = g_fb_pitch / 4;   /* pixels per scanline (= 800 in VBE mode) */
    uint32_t *dst = (uint32_t*)GFX_MEM + dy * pitch + dx;

    for (int y = 0; y < h; y++, dst += pitch, src += src_stride) {
        /* Copy one row as 64-bit pairs for speed */
        int x = 0;
        for (; x + 1 < w; x += 2) {
            uint64_t pair = (uint64_t)src[x] | ((uint64_t)src[x+1] << 32);
            *((uint64_t*)(dst + x)) = pair;
        }
        if (x < w) dst[x] = src[x];
    }
}

void vga_begin_frame(void)
{
    real_gfx_mem = GFX_MEM;   /* save real framebuffer address */
    GFX_MEM      = wm_back_buf;
}

void vga_panic_setup(void)
{
    /* Called by the panic subsystem.  Restores GFX_MEM to the real
       framebuffer so all subsequent pixel writes go directly to screen,
       bypassing the WM double-buffer that may be mid-frame. */
    if (real_gfx_mem)
        GFX_MEM = real_gfx_mem;
    /* If begin_frame was never called, GFX_MEM already points to the
       real framebuffer — nothing to do. */
}

void vga_end_frame(void)
{
    /* Copy back buffer → real screen as fast 32-bit words */
    uint32_t *src = (uint32_t*)wm_back_buf;
    uint32_t *dst = (uint32_t*)real_gfx_mem;
    for (int i = 0; i < 800 * 600; i++) dst[i] = src[i];
    GFX_MEM = real_gfx_mem;   /* restore so cursor draws hit the real screen */
}

void vga_end_frame_cursor(int cx, int cy, int cw, int ch,
                          const uint8_t *shape, uint32_t c_out, uint32_t c_in)
{
    static int blit_count = 0;
    uint32_t t0 = pit_get_ticks();
    if (!real_gfx_mem) return;
    uint32_t *src = (uint32_t*)wm_back_buf;
    uint32_t *dst = (uint32_t*)real_gfx_mem;
    int cy1 = cy + ch;

    for (int y = 0; y < 600; y++) {
        uint32_t *s = src + y * 800;
        uint32_t *d = dst + y * 800;
        if (y < cy || y >= cy1) {
            /* Row outside cursor Y range: fast paired-word bulk copy */
            int i = 0;
            for (; i + 1 < 800; i += 2) *((uint64_t*)(d+i)) = *((uint64_t*)(s+i));
            if (i < 800) d[i] = s[i];
        } else {
            /* Row intersects cursor: copy pixels, composite cursor span inline */
            int ry = y - cy;
            for (int x = 0; x < 800; x++) {
                uint32_t px = s[x];
                if (x >= cx && x < cx + cw) {
                    uint8_t p = shape[ry * cw + (x - cx)];
                    if      (p == 1) px = c_out;
                    else if (p == 2) px = c_in;
                }
                d[x] = px;
            }
        }
    }
    GFX_MEM = real_gfx_mem;
    {
        uint32_t t1 = pit_get_ticks();
        uint32_t dt = t1 - t0;
        blit_count++;
        if (blit_count <= 5 || dt > 2) {
            /* print blit count and duration in ticks (100Hz = 10ms/tick) */
            char buf[48];
            int i = 0;
            buf[i++] = 'B'; buf[i++] = 'L'; buf[i++] = 'I'; buf[i++] = 'T';
            buf[i++] = '#';
            int n = blit_count; if (n > 999) n = 999;
            if (n >= 100) buf[i++] = '0' + n/100;
            if (n >= 10)  buf[i++] = '0' + (n/10)%10;
            buf[i++] = '0' + n%10;
            buf[i++] = ' '; buf[i++] = 'd'; buf[i++] = 't'; buf[i++] = '=';
            int d = (int)dt; if (d > 999) d = 999;
            if (d >= 100) buf[i++] = '0' + d/100;
            if (d >= 10)  buf[i++] = '0' + (d/10)%10;
            buf[i++] = '0' + d%10;
            buf[i++] = '\n'; buf[i] = 0;
            serial_puts(buf);
        }
    }
}

void vga_abort_frame(void)
{
    /* Discard in-progress frame: restore GFX_MEM to real screen without
       blitting the back buffer.  Used to write clean-up pixels into the
       back buffer (via a second begin_frame) without touching the screen. */
    GFX_MEM = real_gfx_mem;
}

void vga_end_frame_partial(int y0, int h)
{
    /* Blit only rows y0..y0+h-1 from back buffer → real screen.
       Used for cheap clock-only taskbar updates without a full scene blit. */
    if (!real_gfx_mem) return;
    uint32_t *src = (uint32_t*)wm_back_buf + y0 * 800;
    uint32_t *dst = (uint32_t*)real_gfx_mem + y0 * 800;
    for (int i = 0; i < 800 * h; i++) dst[i] = src[i];
    GFX_MEM = real_gfx_mem;
}

void vga_blit_frame(void)
{
    /* Re-blit the unchanged back buffer to screen — erases any cursor drawn
     * on the real screen without re-rendering the full scene. */
    if (!real_gfx_mem) return;
    uint32_t *src = (uint32_t*)wm_back_buf;
    uint32_t *dst = (uint32_t*)real_gfx_mem;
    for (int i = 0; i < 800 * 600; i++) dst[i] = src[i];
}

void vga_backbuf_to_screen_rect(int x, int y, int w, int h)
{
    /* Copy a rectangle from back_buf directly to the real framebuffer.
       Used to erase a cursor that is outside the ELF partial-blit rect. */
    if (!real_gfx_mem) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 800) w = 800 - x;
    if (y + h > 600) h = 600 - y;
    if (w <= 0 || h <= 0) return;
    const uint32_t *src = (const uint32_t*)wm_back_buf + y * 800 + x;
    uint32_t       *dst = (uint32_t*)real_gfx_mem      + y * 800 + x;
    for (int row = 0; row < h; row++, src += 800, dst += 800) {
        int col = 0;
        for (; col + 1 < w; col += 2)
            *((uint64_t*)(dst+col)) = *((uint64_t*)(src+col));
        if (col < w) dst[col] = src[col];
    }
}

void vga_cursor_blit(int x, int y, int w, int h,
                     const uint8_t *shape, uint32_t c_out, uint32_t c_in)
{
    if (!real_gfx_mem) return;
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= g_fb_h) continue;
        const uint32_t *bb  = (const uint32_t*)wm_back_buf + py * 800;
        uint32_t       *scr = (uint32_t*)real_gfx_mem      + py * 800;
        /* Build the full row in a local buffer, then write it at once.
           Writing a contiguous block is more atomic to the VGA scan beam
           than scattered single-pixel writes, reducing tearing artefacts. */
        uint32_t row_buf[64];   /* max cursor width supported */
        int cw = w;
        if (x + cw > g_fb_w) cw = g_fb_w - x;
        if (x < 0) cw += x;    /* handled below via px check */
        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < 0 || px >= g_fb_w) continue;
            uint8_t p = shape[row * w + col];
            row_buf[col] = (p == 0) ? bb[px] : (p == 1) ? c_out : c_in;
        }
        /* Copy row to screen — contiguous write per visible column */
        for (int col = 0; col < w; col++) {
            int px = x + col;
            if (px < 0 || px >= g_fb_w) continue;
            scr[px] = row_buf[col];
        }
    }
}

void vga_end_frame_rect(int x, int y, int w, int h)
{
    /* End the frame by blitting only the specified rectangle from back_buf
       to the real screen.  Faster than vga_end_frame when only a sub-region
       changed (e.g. ELF pixel-buffer window during GFX mode). */
    GFX_MEM = real_gfx_mem;
    vga_backbuf_to_screen_rect(x, y, w, h);
}

/* ── Fast drawing helpers using direct row-pointer arithmetic ────────
 * Avoid calling vga_pixel per pixel (bounds check + multiply per call).
 * These assume the caller passes in-bounds coordinates.
 * A shared helper computes the row base pointer and stride. */

static inline uint32_t *row_ptr(int x, int y)
{
    return (uint32_t*)(GFX_MEM + (uint32_t)(y * g_fb_pitch)) + x;
}
static inline int fb_stride(void)
{
    return (int)(g_fb_pitch / 4);
}

void vga_hline(int x, int y, int len, uint32_t color)
{
    if (y < 0 || y >= g_fb_h) return;
    if (x < 0) { len += x; x = 0; }
    if (x + len > g_fb_w) len = g_fb_w - x;
    if (len <= 0) return;
    uint32_t *p = row_ptr(x, y);
    for (int i = 0; i < len; i++) p[i] = color;
}

void vga_vline(int x, int y, int len, uint32_t color)
{
    if (x < 0 || x >= g_fb_w) return;
    if (y < 0) { len += y; y = 0; }
    if (y + len > g_fb_h) len = g_fb_h - y;
    if (len <= 0) return;
    int stride = fb_stride();
    uint32_t *p = row_ptr(x, y);
    for (int i = 0; i < len; i++) { *p = color; p += stride; }
}

void vga_rect(int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_fb_w) w = g_fb_w - x;
    if (y + h > g_fb_h) h = g_fb_h - y;
    if (w <= 0 || h <= 0) return;
    int stride = fb_stride();
    uint32_t *rp = row_ptr(x, y);
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) rp[c] = color;
        rp += stride;
    }
}

void vga_blit_char(int x, int y, char c, uint32_t color)
{
    if (c < 0x20 || c > 0x7F) c = '?';
    const uint8_t *glyph = font8x8[(uint8_t)c - 0x20];
    if (x < 0 || x + CHAR_W > g_fb_w || y < 0 || y + 8 > g_fb_h) return;
    int stride = fb_stride();
    uint32_t *rp = row_ptr(x, y);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < CHAR_W; col++)
            if (bits & (0x80 >> col)) rp[col] = color;
        rp += stride;
    }
}

void vga_print_gfx(int x, int y, const char *str, uint32_t color)
{
    int cx = x;
    while (*str) {
        if (*str == '\n') { cx = x; y += 9; }
        else { vga_blit_char(cx, y, *str, color); cx += 8; }
        str++;
    }
}

void vga_gradient(int x, int y, int w, int h, uint32_t top_rgb, uint32_t bot_rgb)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_fb_w) w = g_fb_w - x;
    if (y + h > g_fb_h) h = g_fb_h - y;
    if (w <= 0 || h <= 0) return;
    int tr = (top_rgb >> 16) & 0xFF, tg = (top_rgb >> 8) & 0xFF, tb = top_rgb & 0xFF;
    int br = (bot_rgb >> 16) & 0xFF, bgc = (bot_rgb >> 8) & 0xFF, bb = bot_rgb & 0xFF;
    int stride = fb_stride();
    uint32_t *rp = row_ptr(x, y);
    for (int row = 0; row < h; row++) {
        int r = (tr * (h - row) + br * row) / h;
        int g = (tg * (h - row) + bgc * row) / h;
        int b = (tb * (h - row) + bb * row) / h;
        uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        for (int col = 0; col < w; col++) rp[col] = c;
        rp += stride;
    }
}

/* ── Off-screen pixel buffer rendering ──────────────────────────────── */

void vga_buf_rect(uint32_t *buf, int bw, int bh,
                  int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++) {
        uint32_t *p = buf + row * bw + x;
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

void vga_buf_hline(uint32_t *buf, int bw, int bh,
                   int x, int y, int len, uint32_t color)
{
    vga_buf_rect(buf, bw, bh, x, y, len, 1, color);
}

void vga_buf_vline(uint32_t *buf, int bw, int bh,
                   int x, int y, int len, uint32_t color)
{
    if (x < 0 || x >= bw) return;
    if (y < 0) { len += y; y = 0; }
    if (y + len > bh) len = bh - y;
    if (len <= 0) return;
    for (int row = y; row < y + len; row++) buf[row * bw + x] = color;
}

void vga_buf_char(uint32_t *buf, int bw, int bh,
                  int x, int y, char c, uint32_t color)
{
    if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7F) c = '?';
    const uint8_t *glyph = font8x8[(uint8_t)c - 0x20];
    for (int row = 0; row < 8; row++) {
        int py = y + row;
        if (py < 0 || py >= bh) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            int px = x + col;
            if (px < 0 || px >= bw) continue;
            buf[py * bw + px] = color;
        }
    }
}

void vga_buf_str(uint32_t *buf, int bw, int bh,
                 int x, int y, const char *s, uint32_t color)
{
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += 9; }
        else { vga_buf_char(buf, bw, bh, cx, y, *s, color); cx += 8; }
        s++;
    }
}

void vga_buf_gradient(uint32_t *buf, int bw, int bh,
                      int x, int y, int w, int h,
                      uint32_t top_rgb, uint32_t bot_rgb)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > bw) w = bw - x;
    if (y + h > bh) h = bh - y;
    if (w <= 0 || h <= 0) return;
    int r1=(top_rgb>>16)&0xFF, g1=(top_rgb>>8)&0xFF, b1=top_rgb&0xFF;
    int r2=(bot_rgb>>16)&0xFF, g2=(bot_rgb>>8)&0xFF, b2=bot_rgb&0xFF;
    for (int row = 0; row < h; row++) {
        int r = (r1*(h-row) + r2*row) / h;
        int g = (g1*(h-row) + g2*row) / h;
        int b = (b1*(h-row) + b2*row) / h;
        uint32_t px = ((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
        uint32_t *p = buf + (y+row)*bw + x;
        for (int col = 0; col < w; col++) p[col] = px;
    }
}

void vga_set_mode3h(void)
{
    /* ── Step 1: Disable Bochs VBE Extension ────────────────────────
       GRUB's graphical menu activates VBE (Bochs VBE ports 0x1CE/0x1CF).
       While VBE_DISPI_ENABLED (index 4, bit 0) is set, QEMU's display
       engine renders from the VBE linear framebuffer and IGNORES standard
       VGA register changes — so the text-mode register writes below have
       no visible effect until VBE is disabled.
       VBE registers are 16-bit, accessed via outw. */
    gfx_outw(0x01CE, 0x0004);  /* select VBE_DISPI_INDEX_ENABLE */
    gfx_outw(0x01CF, 0x0000);  /* write 0 → disable VBE */

    /* ── Step 2: Miscellaneous Output ───────────────────────────────
       0x67 = 28 MHz dot clock, colour output, 400-line (positive vsync). */
    gfx_outb(0x3C2, 0x67);

    /* ── Step 3: Sequencer ──────────────────────────────────────────
       Written directly (no sequencer reset) — same style as vga_set_mode13h.
       SR0=reset-release, SR1=9-dot chars, SR2=planes 0+1, SR4=odd/even. */
    static const uint8_t seq3[5] = { 0x03, 0x00, 0x03, 0x00, 0x02 };
    for (int i = 0; i < 5; i++) {
        gfx_outb(0x3C4, (uint8_t)i);
        gfx_outb(0x3C5, seq3[i]);
    }

    /* ── Step 4: Unlock CRTC registers 0–7 ─────────────────────────
       CR11 bit 7 is the write-protect bit; clear it before writing CR0–CR7. */
    gfx_outb(0x3D4, 0x11);
    gfx_outb(0x3D5, (uint8_t)(gfx_inb(0x3D5) & 0x7F));

    /* ── Step 5: CRTC — standard 80×25 text, 400 scan lines ────────
       CR09[4:0]=0x0F → 16 scan lines per char row; 25 rows × 16 = 400.
       CR0A/0B = 0x0E/0x0F → underscore cursor at bottom 2 scan lines. */
    static const uint8_t crtc3[25] = {
        0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
        0x00, 0x4F, 0x0E, 0x0F, 0x00, 0x00, 0x00, 0x00,
        0x9C, 0x8E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3, 0xFF
    };
    for (int i = 0; i < 25; i++) {
        gfx_outb(0x3D4, (uint8_t)i);
        gfx_outb(0x3D5, crtc3[i]);
    }

    /* ── Step 6: Graphics Controller — odd/even, map at 0xB8000 ───── */
    static const uint8_t gc3[9] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF
    };
    for (int i = 0; i < 9; i++) {
        gfx_outb(0x3CE, (uint8_t)i);
        gfx_outb(0x3CF, gc3[i]);
    }

    /* ── Step 7: Attribute Controller — 16-colour text palette ──────
       AR10 = 0x0C: alphanumeric mode, blink enabled. */
    static const uint8_t ac3[21] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        0x0C, 0x00, 0x0F, 0x08, 0x00
    };
    gfx_inb(0x3DA); /* reset AC flip-flop to index phase */
    for (int i = 0; i < 21; i++) {
        gfx_outb(0x3C0, (uint8_t)i);
        gfx_outb(0x3C0, ac3[i]);
    }
    gfx_outb(0x3C0, 0x20); /* enable video output */

    /* ── Step 8: Reset text driver and clear screen ─────────────── */
    vga_init();
}
