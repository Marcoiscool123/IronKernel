#ifndef VGA_H
#define VGA_H
/* Header guard — prevents this file being included twice in one
   compilation unit. Without it, every double-include causes
   duplicate declaration errors and the build fails. */

#include "../kernel/types.h"
/* IRONKERNEL's own type definitions — no compiler paths, no external
   headers. uint8_t, uint16_t, size_t are defined entirely by us
   in src/kernel/types.h. We own every type in this kernel. */

/* ── VGA DIMENSIONS ─────────────────────────────────────────────── */

#define VGA_WIDTH  80
/* Standard VGA text mode is 80 columns wide. Hardcoded by the VGA
   hardware spec. Writing past column 80 wraps into the next row's
   memory without warning — so we track this and wrap manually. */

#define VGA_HEIGHT 25
/* Standard VGA text mode is 25 rows tall. Row 25 is the last.
   Scrolling is not automatic — we implement it ourselves later. */

/* ── VGA COLOUR CODES ───────────────────────────────────────────── */

typedef enum {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN   = 14,
    VGA_COLOR_WHITE         = 15,
} vga_color_t;
/* Each value maps to a 4-bit colour code defined by the VGA standard.
   The hardware reads these bits directly from the attribute byte
   in video memory. These values are not arbitrary — they are
   mandated by the IBM VGA specification from 1987. */

/* ── PIXEL TERMINAL GEOMETRY ─────────────────────────────────────── */

#define CHAR_W   8                /* rendered glyph width in pixels   */
#define LINE_H   9                /* glyph height + 1 px gap          */
#define PT_COLS  (800 / CHAR_W)   /* 100 chars per line (VBE 800px)   */
#define PT_ROWS  (600 / LINE_H)   /* 66 visible rows    (VBE 600px)   */

/* ── PUBLIC FUNCTION DECLARATIONS ───────────────────────────────── */

void vga_set_fb(uint64_t addr, uint32_t pitch);
/* Tell the VGA driver where the VBE framebuffer is.
   Call with addr/pitch from multiboot2 before vga_init(). */

void vga_scroll_init(void);
/* Reset scrollback state (called once at boot via scroll_init). */
int  vga_scroll_active(void);
/* Returns 1 if user has scrolled back into history. */

void vga_init(void);
/* Clears the screen and sets the cursor to row 0, column 0.
   Must be called before any other vga_ function. */

void vga_putchar(char c);
/* Writes one character to the current cursor position and advances
   the cursor. Handles newline '\n' by moving to the next row. */

void vga_print(const char* str);
/* Writes a null-terminated string to the screen by calling
   vga_putchar for each character until '\0' is reached. */

void vga_set_color(vga_color_t fg, vga_color_t bg);
void vga_backspace(void);
void vga_view_up(void);
/* Scroll view back by 12 rows — shows older scrollback content. */
void vga_view_down(void);
/* Scroll view forward (snap back to live screen). */
void vga_view_reset(void);
/* Immediately restore live screen — call before any keystroke write. */
/* Erase the character immediately before the cursor.
   Moves cursor back one position and writes a space. */
void vga_set_cursor(void);
void vga_goto(uint8_t row, uint8_t col);
/* Move cursor to (row, col) without writing anything. */
void vga_get_cursor(int *row, int *col);
/* Read current cursor position into *row and *col. */
void vga_write_at(uint8_t row, uint8_t col, const char* str,
                  vga_color_t fg, vga_color_t bg);
/* Write str directly into VGA memory at (row,col), padding the rest
   of the row with spaces — cursor position is NOT changed. */
/* Sync the hardware blinking cursor to the current vga_row/vga_col.
   Writes to CRTC registers 0x0E and 0x0F via ports 0x3D4/0x3D5.
   Must be called after every operation that moves the cursor. */
/* Sets the foreground (text) and background colour for all
   subsequent characters written. Default: white on black. */

/* ── VBE 32BPP GRAPHICS DIMENSIONS ──────────────────────────────── */

#define VGA_GFX_WIDTH  800
#define VGA_GFX_HEIGHT 600
/* VBE 32bpp linear framebuffer: 800x600 true colour, 4 bytes per pixel.
   Colours are 0x00RRGGBB packed 32-bit values. */

/* ── VBE 32BPP FUNCTION DECLARATIONS ────────────────────────────── */

void vga_set_mode13h(void);
/* Program VGA hardware registers to enter 320x200 256-colour mode.
   Writes Misc Output, Sequencer, CRTC, Graphics Controller, and
   Attribute Controller registers directly — no BIOS required. */

void vga_gfx_clear(uint32_t color);
/* Fill the entire framebuffer with a single 32bpp colour. */

void vga_redraw(void);
/* Redraw the current terminal state onto the VBE framebuffer.
   Call after a graphics demo to restore the terminal display. */

void vga_pixel(int x, int y, uint32_t color);
/* Write one pixel at (x, y) with a 32bpp colour. Bounds-checked. */

uint32_t vga_read_pixel(int x, int y);
/* Read one 32bpp pixel from the framebuffer at (x, y). Returns 0 if out of range. */

void vga_blit_pixels(int dx, int dy, int w, int h,
                     const uint32_t *src, int src_stride);
/* Fast row-copy blit: write a w×h pixel region from src (row stride=src_stride)
   into the current GFX_MEM at (dx,dy).  Used for blitting ELF pixel buffers. */

void vga_panic_setup(void);
/* Restore GFX_MEM to the real framebuffer (called by the panic subsystem
   to bypass the WM double-buffer and write directly to screen). */

void vga_begin_frame(void);
/* Redirect all pixel writes to the WM back buffer. */
void vga_end_frame(void);
/* Blit back buffer → real screen (atomic), restore GFX_MEM to real screen. */
void vga_end_frame_partial(int y0, int h);
/* Blit only rows y0..y0+h-1 from back buffer → screen (cheap partial update). */
void vga_blit_frame(void);
void vga_abort_frame(void);
/* Discard in-progress frame: restore GFX_MEM to real screen without blitting.
   Use with a second vga_begin_frame() to write pixels into the back buffer
   without making them visible on-screen. */
/* Re-blit existing back buffer to screen without re-rendering (cursor erase). */
void vga_backbuf_to_screen_rect(int x, int y, int w, int h);
/* Copy a rectangle from the back buffer to the real screen (cursor erase helper). */
void vga_cursor_blit(int x, int y, int w, int h,
                     const uint8_t *shape, uint32_t c_out, uint32_t c_in);
/* Composite cursor shape over back-buffer pixels and write full rows to screen.
   shape is row-major w×h: 0=transparent (back buf), 1=outline (c_out), 2=fill (c_in).
   Writes complete rows so the cursor is never partially visible due to tearing. */
void vga_end_frame_rect(int x, int y, int w, int h);
/* End frame blitting only the specified rectangle — much faster than vga_end_frame
   when only a sub-region changed (e.g. ELF pixel-buffer window). */
void vga_end_frame_cursor(int cx, int cy, int cw, int ch,
                          const uint8_t *shape, uint32_t c_out, uint32_t c_in);
/* Blit back buffer → screen compositing the cursor inline per-row.
   Cursor pixels land on screen atomically with the surrounding scene —
   no cursor-absent gap between end_frame and a subsequent cursor_draw. */

void vga_rect(int x, int y, int w, int h, uint32_t color);
/* Fill a solid axis-aligned rectangle with a 32bpp colour. */

void vga_hline(int x, int y, int len, uint32_t color);
/* Draw a horizontal line of 'len' pixels starting at (x, y). */

void vga_vline(int x, int y, int len, uint32_t color);
/* Draw a vertical line of 'len' pixels starting at (x, y). */

void vga_blit_char(int x, int y, char c, uint32_t color);
/* Render one character from the 8x8 IBM VGA font at pixel (x, y).
   Characters outside ASCII 0x20–0x7F are replaced with '?'. */

void vga_print_gfx(int x, int y, const char *str, uint32_t color);
/* Render a null-terminated string using vga_blit_char.
   '\n' resets x and advances y by 9 pixels (8px glyph + 1px gap). */

void vga_gradient(int x, int y, int w, int h, uint32_t top_rgb, uint32_t bot_rgb);
/* Fill a rectangle with a vertical linear gradient from top_rgb to bot_rgb. */

void vga_set_mode3h(void);
/* Restore VGA to standard 80x25 text mode (Mode 3h).
   Programs hardware registers directly, then calls vga_init()
   to reset the text driver state and clear the screen. */

/* ── Off-screen pixel buffer rendering ──────────────────────────── */
/* Render into an arbitrary uint32_t[bh][bw] pixel buffer.
   All functions clip to the buffer bounds. */
void vga_buf_rect(uint32_t *buf, int bw, int bh,
                  int x, int y, int w, int h, uint32_t color);
void vga_buf_hline(uint32_t *buf, int bw, int bh,
                   int x, int y, int len, uint32_t color);
void vga_buf_vline(uint32_t *buf, int bw, int bh,
                   int x, int y, int len, uint32_t color);
void vga_buf_char(uint32_t *buf, int bw, int bh,
                  int x, int y, char c, uint32_t color);
void vga_buf_str(uint32_t *buf, int bw, int bh,
                 int x, int y, const char *s, uint32_t color);
void vga_buf_gradient(uint32_t *buf, int bw, int bh,
                      int x, int y, int w, int h,
                      uint32_t top_rgb, uint32_t bot_rgb);

/* ── Output intercept hooks (set by WM) ─────────────────────────── */
extern void (*vga_print_hook)(const char *s);
extern void (*vga_color_hook)(uint8_t fg, uint8_t bg);

#endif
/* Closes the header guard opened at the top of this file. */
