/*
 * wm.c — Floating window manager for IronKernel.
 *
 * Uses the existing VBE 640×480×8 framebuffer set up by vga_init().
 * Drawing goes through vga_pixel / vga_rect / vga_blit_char / vga_print_gfx.
 * All shell output is intercepted via vga_print_hook → wm_hook_print().
 *
 * Splash screen mimics the MS-DOS "WIN" startup:
 *   black screen → grainy 4-pane flag logo → "IronKernel GUI" title
 *   → wipe-in desktop → floating windows.
 */

#include "wm.h"
#include "../kernel/shell.h"
#include "../kernel/types.h"
#include "../kernel/sched.h"
#include "../drivers/pit.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/fat32.h"
#include "../drivers/elf.h"
#include "../drivers/speaker.h"
#include "../drivers/ac97.h"

/* ── Colour constants — Aero glass palette (32bpp RGB) ───────────── */
#define C_DESKTOP        0x06101E   /* deep space (overridden by gradient)*/
#define C_TITLE_ACT_HI   0x70A8F0   /* glass blue — upper highlight   */
#define C_TITLE_ACT_LO   0x1848A8   /* glass blue — lower deep        */
#define C_TITLE_INACT_HI 0x586878   /* glass grey — upper             */
#define C_TITLE_INACT_LO 0x283038   /* glass grey — lower             */
#define C_TITLE_TXT      0xFFFFFF   /* white title text (with shadow) */
#define C_WIN_BG         0x0C1020   /* dark navy client area          */
#define C_WIN_TXT        0xC8D4E8   /* light blue-grey text           */
#define C_BORDER_HI      0x6090C0   /* glassy blue border highlight   */
#define C_BORDER_LO      0x0C1828   /* deep navy border shadow        */
#define C_CLOSE          0xC02828   /* vivid red close                */
#define C_TASKBAR        0x080E1A   /* near-black taskbar base        */
#define C_TASKBAR_TXT    0x90B0D0   /* cool blue-grey taskbar text    */
#define C_CURSOR         0xFFFFFF   /* white cursor                   */
#define C_CURSOR_OUT     0x000000   /* black cursor outline           */

/* Title bar / border geometry */
#define TITLE_H   18   /* px — title bar height           */
#define BORDER     1   /* px — border each side           */
#define CLOSE_SZ  12   /* px — close-button size          */

/* Taskbar strip at the very bottom */
#define TBAR_Y   580
#define TBAR_H    20

/* Start button */
#define START_BTN_X   4
#define START_BTN_W  52

/* Start menu popup */
#define POPUP_W      160
#define POPUP_ITEM_H  12
#define START_SYS_COUNT 6
#define POWER_ITEM_COUNT 3

/* Taskbar window buttons (start after start button) */
#define TBTN_W   90    /* px per button                  */
#define TBTN_GAP  4    /* px gap between buttons         */
#define TBTN_X0  60    /* x of first button (after start btn) */

/* Character cell: 8 wide, 9 tall (CHAR_W=8, LINE_H=9 from vga.h) */
#define CW   8
#define CH   9

/* Screen dimensions defined in wm.h (SCR_W=800, SCR_H=600) */

/* ── Globals ──────────────────────────────────────────────────────── */
wm_win_t wm_wins[WM_MAX_WIN];
int      wm_focused  = -1;
uint32_t wm_cur_fg   = C_WIN_TXT;
int      wm_is_running = 0;

/* ELF pixel-buffer GFX — one global buffer; only one ELF runs at a time */
uint32_t     wm_elf_gfx_buf[WM_ELF_GFX_MAXH * WM_ELF_GFX_MAXW];
int          wm_elf_gfx_cw     = 0;
int          wm_elf_gfx_ch     = 0;
volatile int wm_elf_gfx_active = 0;

/* Shell window id — set in wm_run. Hook always routes output here,
   regardless of which window is visually focused. */
static int wm_shell_id = -1;
static int wm_demo_id  = -1;   /* color demo window — rendered with pixel ops */

/* Visual Z-top: the window drawn last (visually on top).
   Decoupled from wm_focused (keyboard) so ELF windows can appear
   in front without stealing keyboard focus from the shell. */
int wm_z_top = -1;

/* Mutex: only one user_mode_enter may be active at a time */
static volatile int elf_running = 0;

/* Forward declaration needed by wm_shell_cmd_task below */
static void print_prompt(int win);

/* Background shell command task — spawned so the WM loop stays live */
static char            wm_cmd_buf[256];
static volatile int    wm_cmd_running = 0;

static void wm_shell_cmd_task(void)
{
    shell_dispatch(wm_cmd_buf);
    /* Print prompt directly from this task — reliable even if WM loop
       misses the wm_cmd_running transition due to scheduling races. */
    if (wm_shell_id >= 0) {
        wm_putchar(wm_shell_id, '\n');
        print_prompt(wm_shell_id);
        wm_wins[wm_shell_id].dirty = 1;
    }
    wm_cmd_running = 0;
    task_exit();
}

/* Explicit output window for the currently-running ELF task.
   Set by elf_task_trampoline before elf_exec, cleared after.
   Avoids relying on sched_current_id() which can race with the PIT. */
static volatile int wm_elf_out_win = -1;

/* ── Start menu state ─────────────────────────────────────────────── */
static volatile int   start_open      = 0;
/* 0=none  1=exit-gui  2=shutdown  3=reboot — set by popup_handle_click */
static volatile int   wm_power_action = 0;
#define MAX_START_ELFS 8
static fat32_dentry_t start_elfs[MAX_START_ELFS];
static int            start_elf_count = 0;

static const char *start_sys_cmds[START_SYS_COUNT] = {
    "shell", "help", "ls", "ps", "fsinfo", "clear"
};
static const char *start_sys_labels[START_SYS_COUNT] = {
    " Open Shell", " Help", " List Files", " Processes", " FS Info", " Clear Screen"
};

/* ── Forward declarations ─────────────────────────────────────────── */
static void draw_border_3d(int x, int y, int w, int h, int raised);
static void print_prompt(int win);

/* ── vga_print_hook / vga_color_hook (set by wm_run) ─────────────── */
static void wm_hook_print(const char *s);
static void wm_hook_color(uint8_t fg, uint8_t bg);

/* ── Helpers ─────────────────────────────────────────────────────── */
static int str_len_wm(const char *s)
{ int n=0; while(s[n]) n++; return n; }

static void mem_set(void *dst, uint8_t v, int n)
{ uint8_t *p = (uint8_t*)dst; while(n-->0) *p++=v; }

static void str_copy_wm(char *d, const char *s, int max)
{ int i=0; while(i<max-1&&s[i]){d[i]=s[i];i++;} d[i]='\0'; }

/* Clear a window's text buffer — used instead of vga_clear() in GUI mode */
static void wm_clear_win(int id)
{
    if (id < 0 || id >= WM_MAX_WIN || !wm_wins[id].alive) return;
    wm_win_t *wp = &wm_wins[id];
    for (int r = 0; r < WM_BUF_ROWS; r++)
        for (int c = 0; c < WM_BUF_COLS; c++) {
            wp->buf[r][c] = ' ';
            wp->col[r][c] = C_WIN_TXT;
        }
    wp->cur_col = 0;
    wp->cur_row = 0;
    wp->top_row = 0;
    wp->dirty   = 1;
}

/* ── Start menu helpers ───────────────────────────────────────────── */
static void load_start_elfs(void)
{
    static fat32_dentry_t tmp[64];
    int n = fat32_readdir(fat32_cwd_cluster, tmp, 64);
    start_elf_count = 0;
    for (int i = 0; i < n && start_elf_count < MAX_START_ELFS; i++) {
        if (!tmp[i].is_dir &&
            tmp[i].ext[0]=='E' && tmp[i].ext[1]=='L' && tmp[i].ext[2]=='F')
            start_elfs[start_elf_count++] = tmp[i];
    }
}

static int popup_h(void)
{
    int u = (start_elf_count > 0) ? start_elf_count : 1;
    return 4 + 10 + START_SYS_COUNT * POPUP_ITEM_H
             + 6 + 10 + u * POPUP_ITEM_H
             + 6 + 10 + POWER_ITEM_COUNT * POPUP_ITEM_H + 4;
}

/* Open (or focus) the shell window */
static void wm_open_shell(void)
{
    /* If already open, just focus it */
    if (wm_shell_id >= 0 && wm_wins[wm_shell_id].alive) {
        wm_wins[wm_shell_id].minimized = 0;
        wm_focused = wm_shell_id;
        return;
    }
    int ns = wm_create(6, 28, 554, 546, "IronKernel Shell");
    if (ns < 0) return;
    wm_shell_id = ns;
    wm_focused  = ns;
    wm_cur_fg   = C_WIN_TXT;
    wm_puts(ns, " IronKernel GUI Shell\n");
    wm_puts(ns, " Type 'help' for commands.\n");
    wm_puts(ns, " Type 'exit' to return to text shell.\n\n");
    print_prompt(ns);
}

/* rel_y = click y relative to popup top */
static void popup_handle_click(int rel_y)
{
    rel_y -= 4;
    if (rel_y < 0) return;
    if (rel_y < 10) return;          /* System Apps header */
    rel_y -= 10;
    if (rel_y < START_SYS_COUNT * POPUP_ITEM_H) {
        int idx = rel_y / POPUP_ITEM_H;
        const char *cmd = start_sys_cmds[idx];
        /* "shell" → open/focus shell window */
        if (cmd[0]=='s' && cmd[1]=='h' && cmd[2]=='e' && cmd[3]=='l' &&
            cmd[4]=='l' && cmd[5]=='\0') {
            wm_open_shell();
        /* "clear" must clear the window buffer, not the VGA terminal */
        } else if (cmd[0]=='c' && cmd[1]=='l' && cmd[2]=='e' && cmd[3]=='a' &&
                   cmd[4]=='r' && cmd[5]=='\0') {
            wm_clear_win(wm_shell_id);
            if (wm_shell_id >= 0) { wm_puts(wm_shell_id, "\n"); print_prompt(wm_shell_id); }
        } else {
            shell_dispatch((char *)cmd);
            if (wm_shell_id >= 0) {
                wm_puts(wm_shell_id, "\n");
                print_prompt(wm_shell_id);
            }
        }
        return;
    }
    rel_y -= START_SYS_COUNT * POPUP_ITEM_H;
    if (rel_y < 6)  return;          /* separator */
    rel_y -= 6;
    if (rel_y < 10) return;          /* User Made header */
    rel_y -= 10;
    int elf_block = (start_elf_count > 0) ? start_elf_count : 1;
    if (rel_y < elf_block * POPUP_ITEM_H) {
        if (start_elf_count > 0) {
            int idx = rel_y / POPUP_ITEM_H;
            if (idx >= 0 && idx < start_elf_count) {
                char cmd[32];
                cmd[0]='e'; cmd[1]='x'; cmd[2]='e'; cmd[3]='c'; cmd[4]=' ';
                int ci = 5;
                for (int k = 0; start_elfs[idx].name[k] && k < 8; k++)
                    cmd[ci++] = start_elfs[idx].name[k];
                cmd[ci++] = '.';
                for (int k = 0; start_elfs[idx].ext[k] && k < 3; k++)
                    cmd[ci++] = start_elfs[idx].ext[k];
                cmd[ci] = 0;
                wm_spawn_elf(cmd);
            }
        }
        return;
    }
    rel_y -= elf_block * POPUP_ITEM_H;
    if (rel_y < 6)  return;          /* separator before power */
    rel_y -= 6;
    if (rel_y < 10) return;          /* Power header */
    rel_y -= 10;
    if (rel_y < POWER_ITEM_COUNT * POPUP_ITEM_H) {
        int idx = rel_y / POPUP_ITEM_H;
        if (idx == 0) wm_power_action = 1;   /* Exit GUI Mode */
        if (idx == 1) wm_power_action = 2;   /* Shutdown      */
        if (idx == 2) wm_power_action = 3;   /* Reboot        */
    }
}

static void draw_start_menu(void)
{
    int ph = popup_h();
    int py = TBAR_Y - ph;
    int px = START_BTN_X;

    /* Drop shadow */
    vga_rect(px + 5, py + 5, POPUP_W, ph, 0x010204);

    vga_gradient(px, py, POPUP_W, ph, 0x141E30, 0x0A1220);
    vga_hline(px, py, POPUP_W, 0x4878B8);       /* glassy top border */
    vga_hline(px, py+1, POPUP_W, 0x1A2C48);
    draw_border_3d(px, py, POPUP_W, ph, 1);

    int iy = py + 4;

    /* ── System Apps ── */
    vga_print_gfx(px + 4, iy, "System Apps", 0x4488CC);
    iy += 10;
    for (int i = 0; i < START_SYS_COUNT; i++) {
        vga_print_gfx(px + 8, iy + 2, start_sys_labels[i], 0xC0C0D0);
        iy += POPUP_ITEM_H;
    }

    /* ── Separator ── */
    vga_hline(px + 4, iy + 2, POPUP_W - 8, 0x30304A);
    iy += 6;

    /* ── User Made ── */
    vga_print_gfx(px + 4, iy, "User Made", 0x44CC88);
    iy += 10;
    if (start_elf_count == 0) {
        vga_print_gfx(px + 8, iy + 2, "(no ELFs found)", 0x555568);
        iy += POPUP_ITEM_H;
    } else {
        for (int i = 0; i < start_elf_count; i++) {
            char lbl[13];
            int li = 0;
            for (int k = 0; start_elfs[i].name[k] && k < 8; k++)
                lbl[li++] = start_elfs[i].name[k];
            lbl[li++] = '.';
            for (int k = 0; start_elfs[i].ext[k] && k < 3; k++)
                lbl[li++] = start_elfs[i].ext[k];
            lbl[li] = 0;
            vga_print_gfx(px + 8, iy + 2, lbl, 0xC0C0D0);
            iy += POPUP_ITEM_H;
        }
    }

    /* ── Power ── */
    vga_hline(px + 4, iy + 2, POPUP_W - 8, 0x30304A);
    iy += 6;
    vga_print_gfx(px + 4, iy, "Power", 0xCC4444);
    iy += 10;
    vga_print_gfx(px + 8, iy + 2, " Exit GUI Mode", 0xC0C0D0);
    iy += POPUP_ITEM_H;
    vga_print_gfx(px + 8, iy + 2, " Shutdown", 0xC0C0D0);
    iy += POPUP_ITEM_H;
    vga_print_gfx(px + 8, iy + 2, " Reboot", 0xC0C0D0);
}

/* ── wm_create ────────────────────────────────────────────────────── */
int wm_create(int x, int y, int w, int h, const char *title)
{
    for (int i = 0; i < WM_MAX_WIN; i++) {
        if (wm_wins[i].alive) continue;
        wm_win_t *wp = &wm_wins[i];
        mem_set(wp, 0, sizeof(wm_win_t));
        wp->alive   = 1;
        wp->x = x; wp->y = y; wp->w = w; wp->h = h;
        str_copy_wm(wp->title, title, 32);

        /* Visible character grid inside the border + title bar */
        int cx_px = x + BORDER;
        int cy_px = y + TITLE_H + BORDER;
        int cw_px = w - 2 * BORDER;
        int ch_px = h - TITLE_H - 2 * BORDER;
        (void)cx_px; (void)cy_px;
        wp->vis_cols  = cw_px / CW;
        wp->vis_rows  = ch_px / CH;
        if (wp->vis_cols > WM_BUF_COLS) wp->vis_cols = WM_BUF_COLS;
        if (wp->vis_rows > WM_BUF_ROWS) wp->vis_rows = WM_BUF_ROWS;

        wp->cur_col  = 0;
        wp->cur_row  = 0;
        wp->top_row  = 0;
        wp->dirty    = 0;
        return i;
    }
    return -1;
}

/* ── wm_putchar ───────────────────────────────────────────────────── */
void wm_putchar(int id, char c)
{
    if (id < 0 || id >= WM_MAX_WIN || !wm_wins[id].alive) return;
    wm_win_t *wp = &wm_wins[id];

    if (c == '\b') {
        if (wp->cur_col > 0) {
            wp->cur_col--;
            wp->buf[wp->cur_row][wp->cur_col] = ' ';
            wp->col[wp->cur_row][wp->cur_col] = C_WIN_TXT;
        }
        wp->dirty = 1;
        return;
    } else if (c == '\x01') {   /* cursor left, no erase */
        if (wp->cur_col > 0) wp->cur_col--;
        wp->dirty = 1;
        return;
    } else if (c == '\x02') {   /* cursor right, no erase */
        if (wp->cur_col < WM_BUF_COLS - 1) wp->cur_col++;
        wp->dirty = 1;
        return;
    } else if (c == '\n') {
        wp->cur_col = 0;
        wp->cur_row++;
    } else if (c == '\r') {
        wp->cur_col = 0;
    } else if (c == '\t') {
        int next = (wp->cur_col + 8) & ~7;
        while (wp->cur_col < next) {
            if (wp->cur_col < WM_BUF_COLS) {
                wp->buf[wp->cur_row][wp->cur_col] = ' ';
                wp->col[wp->cur_row][wp->cur_col] = wm_cur_fg;
            }
            wp->cur_col++;
        }
        return;
    } else {
        if (wp->cur_col >= WM_BUF_COLS) {
            wp->cur_col = 0;
            wp->cur_row++;
        }
        wp->buf[wp->cur_row][wp->cur_col] = c;
        wp->col[wp->cur_row][wp->cur_col] = wm_cur_fg;
        wp->cur_col++;
    }

    /* Scroll: if cur_row is past the last buffer row, rotate buffer */
    if (wp->cur_row >= WM_BUF_ROWS) {
        /* Shift rows up by 1 */
        for (int r = 0; r < WM_BUF_ROWS - 1; r++) {
            for (int c = 0; c < WM_BUF_COLS; c++) {
                wp->buf[r][c] = wp->buf[r+1][c];
                wp->col[r][c] = wp->col[r+1][c];
            }
        }
        /* Clear last row */
        for (int c = 0; c < WM_BUF_COLS; c++) {
            wp->buf[WM_BUF_ROWS-1][c] = ' ';
            wp->col[WM_BUF_ROWS-1][c] = C_WIN_TXT;
        }
        wp->cur_row = WM_BUF_ROWS - 1;
    }

    /* Keep top_row so cursor is always visible */
    if (wp->cur_row >= wp->top_row + wp->vis_rows)
        wp->top_row = wp->cur_row - wp->vis_rows + 1;
    if (wp->top_row < 0) wp->top_row = 0;

    wp->dirty = 1;   /* signal WM loop: this window needs a redraw */
}

void wm_puts(int id, const char *s)
{ while (*s) wm_putchar(id, *s++); }

void wm_puts_col(int id, const char *s, uint32_t color)
{
    uint32_t saved = wm_cur_fg;
    wm_cur_fg = color;
    wm_puts(id, s);
    wm_cur_fg = saved;
}

void wm_backspace(int id)
{
    if (id < 0 || id >= WM_MAX_WIN || !wm_wins[id].alive) return;
    wm_win_t *wp = &wm_wins[id];
    if (wp->cur_col > 0) {
        wp->cur_col--;
        wp->buf[wp->cur_row][wp->cur_col] = ' ';
        wp->col[wp->cur_row][wp->cur_col] = C_WIN_TXT;
    }
}

/* ── vga hooks ────────────────────────────────────────────────────── */
static void wm_hook_print(const char *s)
{
    /* Route to the ELF's window if one is active (set explicitly by
       elf_task_trampoline — no sched_current_id() race).
       If window was closed, discard silently.
       Otherwise fall back to the shell window. */
    int win = wm_elf_out_win;
    if (win >= 0) {
        if (win < WM_MAX_WIN && wm_wins[win].alive)
            wm_puts(win, s);
        /* else: ELF window closed — discard */
    } else {
        if (wm_shell_id >= 0)
            wm_puts(wm_shell_id, s);
    }
}
static void wm_hook_color(uint8_t fg, uint8_t bg)
{
    (void)bg;
    static const uint32_t pal[] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
    };
    wm_cur_fg = pal[fg & 0x0F];
    if (wm_cur_fg == 0x000000) wm_cur_fg = C_WIN_TXT;
}

/* ── Drawing ─────────────────────────────────────────────────────── */

/* HSV hue (0-359) → 32bpp RGB, full saturation + value */
static uint32_t wm_hue(int h)
{
    int seg = h / 60;
    int f   = (h % 60) * 255 / 59;
    uint32_t r, g, b;
    switch (seg) {
        case 0: r=255;      g=(uint32_t)f;  b=0;        break;
        case 1: r=255-f;    g=255;          b=0;        break;
        case 2: r=0;        g=255;          b=(uint32_t)f; break;
        case 3: r=0;        g=255-f;        b=255;      break;
        case 4: r=(uint32_t)f; g=0;         b=255;      break;
        default:r=255;      g=0;            b=255-f;    break;
    }
    return (r << 16) | (g << 8) | b;
}

/* Pixel content for the Color Demo window client area */
static void draw_demo_win_content(int cx, int cy, int cw, int ch)
{
    (void)ch;

    /* ── Rainbow hue sweep ── */
    int ry = cy + 4;
    for (int x = 0; x < cw; x++)
        vga_vline(cx + x, ry, 26, wm_hue(x * 359 / (cw - 1)));
    vga_print_gfx(cx + 2, ry + 28, "360 deg hue spectrum", 0x8888A8);

    /* ── Gradient bands ── */
    int gy = ry + 40;
    vga_gradient(cx, gy,      cw, 20, 0xFF2020, 0x2020FF);
    vga_gradient(cx, gy + 22, cw, 20, 0x20FF20, 0xFF20FF);
    vga_gradient(cx, gy + 44, cw, 20, 0xFFFF00, 0x00FFFF);
    vga_gradient(cx, gy + 66, cw, 20, 0x000000, 0xFFFFFF);
    vga_print_gfx(cx + 3, gy +  5, "R->B", 0xFFFFFF);
    vga_print_gfx(cx + 3, gy + 27, "G->M", 0xFFFFFF);
    vga_print_gfx(cx + 3, gy + 49, "Y->C", 0x202020);
    vga_print_gfx(cx + 3, gy + 71, "Bk->Wh", 0x888888);

    /* ── 8 solid color swatches ── */
    static const uint32_t sw[8] = {
        0xFF0000, 0xFF8800, 0xFFFF00, 0x00FF00,
        0x00FFFF, 0x0000FF, 0xFF00FF, 0xFFFFFF,
    };
    int sw_y  = gy + 92;
    int sw_w  = cw / 4;
    for (int i = 0; i < 8; i++) {
        int sx = cx + (i % 4) * sw_w;
        int sy = sw_y + (i / 4) * 26;
        vga_rect(sx, sy, sw_w - 1, 24, sw[i]);
    }

    /* ── Info line ── */
    vga_print_gfx(cx + 2, sw_y + 56, "800x600  32bpp  VBE", 0x6688AA);
}

/* 3-D raised box border around rect (x,y,w,h) */
static void draw_border_3d(int x, int y, int w, int h, int raised)
{
    uint32_t hi = raised ? C_BORDER_HI : C_BORDER_LO;
    uint32_t lo = raised ? C_BORDER_LO : C_BORDER_HI;
    /* top edge */    vga_hline(x,       y,       w,   hi);
    /* left edge */   vga_vline(x,       y,       h,   hi);
    /* bottom edge */ vga_hline(x,       y+h-1,   w,   lo);
    /* right edge */  vga_vline(x+w-1,   y,       h,   lo);
}

static void draw_window(int id)
{
    wm_win_t *wp = &wm_wins[id];
    if (!wp->alive || wp->minimized) return;

    int x = wp->x, y = wp->y, w = wp->w, h = wp->h;
    int focused = (id == wm_focused);

    /* ── Drop shadow ── */
    vga_rect(x+4, y+4, w, h, 0x020406);

    /* ── Focused outer glow (2-layer colored ring outside window rect) ── */
    if (focused) {
        vga_hline(x-1, y-1, w+2, 0x2858A8);
        vga_vline(x-1, y,   h,   0x2858A8);
        vga_hline(x-1, y+h, w+2, 0x2858A8);
        vga_vline(x+w, y,   h,   0x2858A8);
        vga_hline(x-2, y-2, w+4, 0x101E38);
        vga_vline(x-2, y-1, h+2, 0x101E38);
        vga_hline(x-2, y+h+1, w+4, 0x101E38);
        vga_vline(x+w+1, y-1, h+2, 0x101E38);
    }

    /* ── Outer border (raised 3-D) ── */
    draw_border_3d(x, y, w, h, 1);

    /* ── Title bar — Aero glass multi-pass ── */
    if (focused) {
        /* Gloss highlight: top 2px near-white */
        vga_hline(x+1, y+1, w-2, 0xC8E0FF);
        vga_hline(x+1, y+2, w-2, 0x90B8F8);
        /* Upper glass face: bright → deep blue */
        vga_gradient(x+1, y+3,  w-2, 8, 0x6898E8, 0x2858C0);
        /* Lower glass reflection: deep → slightly lighter */
        vga_gradient(x+1, y+11, w-2, 6, 0x2050A8, 0x3868C8);
        /* Bottom edge bright line */
        vga_hline(x+1, y+17, w-2, 0x5888D8);
    } else {
        /* Inactive glass — desaturated blue-grey */
        vga_hline(x+1, y+1, w-2, 0xA0B0C0);
        vga_hline(x+1, y+2, w-2, 0x708090);
        vga_gradient(x+1, y+3,  w-2, 8, 0x506070, 0x2C3840);
        vga_gradient(x+1, y+11, w-2, 6, 0x283038, 0x3A4858);
        vga_hline(x+1, y+17, w-2, 0x485868);
    }

    /* Minimize button — glassy */
    {
        int mbx = x + w - 2 * CLOSE_SZ - 4;
        int bh2 = (TITLE_H - 3) / 2;
        vga_gradient(mbx, y+2, CLOSE_SZ, bh2,            0x5888D8, 0x2858B8);
        vga_gradient(mbx, y+2+bh2, CLOSE_SZ, TITLE_H-3-bh2, 0x1840A0, 0x3060C0);
        vga_hline(mbx, y+2, CLOSE_SZ, 0x90C0FF);   /* gloss top */
        draw_border_3d(mbx, y+2, CLOSE_SZ, TITLE_H-3, 1);
        vga_hline(mbx + 3, y + 2 + TITLE_H - 6, 6, 0xFFFFFF);
    }

    /* Close button — glassy red */
    {
        int cbx = x + w - CLOSE_SZ - 2;
        int bh2 = (TITLE_H - 3) / 2;
        vga_gradient(cbx, y+2, CLOSE_SZ, bh2,            0xFF6060, 0xC02020);
        vga_gradient(cbx, y+2+bh2, CLOSE_SZ, TITLE_H-3-bh2, 0x901010, 0xC03030);
        vga_hline(cbx, y+2, CLOSE_SZ, 0xFFA0A0);   /* gloss top */
        draw_border_3d(cbx, y+2, CLOSE_SZ, TITLE_H-3, 1);
        /* X mark */
        int bx = cbx + 3;
        int by = y + 4;
        vga_pixel(bx,   by,   0xFFFFFF); vga_pixel(bx+5, by,   0xFFFFFF);
        vga_pixel(bx+1, by+1, 0xFFFFFF); vga_pixel(bx+4, by+1, 0xFFFFFF);
        vga_pixel(bx+2, by+2, 0xFFFFFF); vga_pixel(bx+3, by+2, 0xFFFFFF);
        vga_pixel(bx+2, by+3, 0xFFFFFF); vga_pixel(bx+3, by+3, 0xFFFFFF);
        vga_pixel(bx+1, by+4, 0xFFFFFF); vga_pixel(bx+4, by+4, 0xFFFFFF);
        vga_pixel(bx,   by+5, 0xFFFFFF); vga_pixel(bx+5, by+5, 0xFFFFFF);
    }

    /* Title text — with drop shadow for glass depth */
    int tlen = str_len_wm(wp->title) * CW;
    int tx   = x + (w - tlen) / 2;
    if (tx < x + CLOSE_SZ + 4) tx = x + CLOSE_SZ + 4;
    int ty   = y + (TITLE_H - 8) / 2;
    vga_print_gfx(tx+1, ty+1, wp->title, 0x00101C); /* shadow */
    vga_print_gfx(tx,   ty,   wp->title, focused ? 0xFFFFFF : 0xC8D4E0);

    /* ── Client area ── */
    int cx = x + BORDER;
    int cy = y + TITLE_H;
    int cw = w - 2 * BORDER;
    int ch = h - TITLE_H - BORDER;

    /* ── ELF pixel-buffer window: blit gfx buf instead of text ── */
    if (wm_elf_gfx_active && id == (int)wm_elf_out_win) {
        int bw = (cw < wm_elf_gfx_cw) ? cw : wm_elf_gfx_cw;
        int bh = (ch < wm_elf_gfx_ch) ? ch : wm_elf_gfx_ch;
        vga_blit_pixels(cx, cy, bw, bh, wm_elf_gfx_buf, wm_elf_gfx_cw);
        goto draw_corners;
    }

    vga_gradient(cx, cy, cw, ch, 0x0E1422, 0x080C16);

    /* ── Color demo window: pixel content, no text buffer ── */
    if (id == wm_demo_id) {
        draw_demo_win_content(cx, cy, cw, ch);
        goto draw_corners;
    }

    /* ── Text content ── */
    for (int r = 0; r < wp->vis_rows; r++) {
        int row = wp->top_row + r;
        if (row >= WM_BUF_ROWS) break;
        for (int c = 0; c < wp->vis_cols; c++) {
            char ch_c = wp->buf[row][c];
            if (!ch_c || ch_c == ' ') continue;
            int px = cx + c * CW;
            int py = cy + r * CH + 1;
            vga_blit_char(px, py, ch_c, wp->col[row][c]);
        }
    }

    /* ── Cursor (blinking simulation — always shown) ── */
    if (focused) {
        int vcur_r = wp->cur_row - wp->top_row;
        int vcur_c = wp->cur_col;
        if (vcur_r >= 0 && vcur_r < wp->vis_rows && vcur_c < wp->vis_cols) {
            int px = cx + vcur_c * CW;
            int py = cy + vcur_r * CH + 8;
            vga_hline(px, py, CW, C_WIN_TXT);
        }
    }

draw_corners:;
    /* ── Rounded corners ── */
    {
        uint32_t cc = 0x020406;
        vga_pixel(x,     y,     cc); vga_pixel(x+1,   y,     cc); vga_pixel(x,     y+1,   cc);
        vga_pixel(x+w-1, y,     cc); vga_pixel(x+w-2, y,     cc); vga_pixel(x+w-1, y+1,   cc);
        vga_pixel(x,     y+h-1, cc); vga_pixel(x+1,   y+h-1, cc); vga_pixel(x,     y+h-2, cc);
        vga_pixel(x+w-1, y+h-1, cc); vga_pixel(x+w-2, y+h-1, cc); vga_pixel(x+w-1, y+h-2, cc);
    }
}

static void draw_desktop(void)
{
    /* Background — multi-band aurora-space gradient */
    vga_gradient(0,   0, SCR_W, 150, 0x06101E, 0x04081A);  /* upper: midnight blue  */
    vga_gradient(0, 150, SCR_W,  80, 0x041416, 0x020C10);  /* aurora: deep teal     */
    vga_gradient(0, 230, SCR_W, 150, 0x080618, 0x050412);  /* nebula: faint purple  */
    vga_gradient(0, 380, SCR_W, 200, 0x020408, 0x010204);  /* lower: near black     */

    /* Colored star field (deterministic LCG — same positions every frame) */
    {
        static const uint32_t star_colors[7] = {
            0xC0D0E8,   /* cool white  */
            0xC0D0E8,   /* cool white  */
            0xC0D0E8,   /* cool white  */
            0x8898FF,   /* icy blue    */
            0xFFE898,   /* warm yellow */
            0xFF9870,   /* orange      */
            0x70B0FF,   /* cyan-blue   */
        };
        uint32_t r = 0xBEEFCAFE;
        for (int i = 0; i < 200; i++) {
            r = r * 1664525u + 1013904223u;
            int sx = (int)((r >> 15) % SCR_W);
            r = r * 1664525u + 1013904223u;
            int sy = (int)((r >> 15) % (TBAR_Y - 8));
            uint8_t br = (uint8_t)(35 + ((r >> 6) & 0x7F));
            int ci = (int)((r >> 22) & 0x07);
            if (ci >= 7) ci = 0;
            uint32_t sc = star_colors[ci];
            uint8_t sr = (uint8_t)(((sc >> 16) & 0xFF) * (uint32_t)br / 255u);
            uint8_t sg = (uint8_t)(((sc >>  8) & 0xFF) * (uint32_t)br / 255u);
            uint8_t sb = (uint8_t)(((sc      ) & 0xFF) * (uint32_t)br / 255u);
            uint32_t col = ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | sb;
            vga_pixel(sx, sy, col);
            /* ~1 in 32 stars gets a sparkle cross */
            if ((r & 0x1F) < 2) {
                uint32_t dim = (col >> 1) & 0x7F7F7Fu;
                if (sx > 0)        vga_pixel(sx-1, sy, dim);
                if (sx < SCR_W-1)  vga_pixel(sx+1, sy, dim);
                if (sy > 0)        vga_pixel(sx, sy-1, dim);
            }
        }
    }

    /* Bottom taskbar — Aero glass */
    vga_gradient(0, TBAR_Y,   SCR_W, TBAR_H/2,    0x182840, 0x0C1828);
    vga_gradient(0, TBAR_Y+TBAR_H/2, SCR_W, TBAR_H-TBAR_H/2, 0x0A1420, 0x141E30);
    vga_hline(0, TBAR_Y,   SCR_W, 0x4878B8);   /* bright glassy top border */
    vga_hline(0, TBAR_Y+1, SCR_W, 0x1A2840);   /* dark shadow below it     */

    /* ── START button — glass orb style ── */
    {
        int bh  = TBAR_H - 4;
        int bh2 = bh / 2;
        int by0 = TBAR_Y + 2;
        /* Upper half: bright gloss */
        vga_gradient(START_BTN_X, by0,      START_BTN_W, bh2,
                     start_open ? 0x80B0FF : 0x5888D8,
                     start_open ? 0x3868D0 : 0x2858B8);
        /* Lower half: darker reflection */
        vga_gradient(START_BTN_X, by0+bh2,  START_BTN_W, bh-bh2,
                     start_open ? 0x2050B8 : 0x1840A0,
                     start_open ? 0x4070D8 : 0x3060C0);
        /* Top gloss line */
        vga_hline(START_BTN_X, by0, START_BTN_W,
                  start_open ? 0xB0D0FF : 0x80B0FF);
        draw_border_3d(START_BTN_X, by0, START_BTN_W, bh, 1);
        vga_print_gfx(START_BTN_X + 8, TBAR_Y + 6, "START", 0xFFFFFF);
    }

    /* Taskbar area separator after START button */
    vga_vline(START_BTN_X + START_BTN_W + 2, TBAR_Y + 2, TBAR_H - 4, 0x1A2840);
    vga_vline(START_BTN_X + START_BTN_W + 3, TBAR_Y + 2, TBAR_H - 4, 0x304858);

    /* Framed clock area */
    {
        int ck_x = SCR_W - 50, ck_y = TBAR_Y + 2, ck_w = 46, ck_h = TBAR_H - 4;
        /* Separator before clock */
        vga_vline(SCR_W - 56, TBAR_Y + 2, TBAR_H - 4, 0x1A2840);
        vga_vline(SCR_W - 55, TBAR_Y + 2, TBAR_H - 4, 0x304858);
        /* Recessed glass box */
        vga_gradient(ck_x, ck_y, ck_w, ck_h, 0x0C1828, 0x182438);
        draw_border_3d(ck_x, ck_y, ck_w, ck_h, 0);   /* sunken */
        /* Uptime MM:SS */
        uint32_t ticks = pit_get_ticks();
        uint32_t secs  = ticks / 100;
        uint32_t mins  = secs  / 60;
        secs %= 60;
        char tbuf[8];
        tbuf[0] = '0' + (char)((mins / 10) % 10);
        tbuf[1] = '0' + (char)(mins % 10);
        tbuf[2] = ':';
        tbuf[3] = '0' + (char)(secs / 10);
        tbuf[4] = '0' + (char)(secs % 10);
        tbuf[5] = '\0';
        vga_print_gfx(ck_x + 5, TBAR_Y + 6, tbuf, C_TASKBAR_TXT);
    }

    /* ── Taskbar window buttons ── */
    int bx = TBTN_X0;
    for (int i = 0; i < WM_MAX_WIN; i++) {
        wm_win_t *wp = &wm_wins[i];
        if (!wp->alive) continue;
        if (bx + TBTN_W > SCR_W - 52) break;
        int focused  = (i == wm_focused);
        int is_min   = wp->minimized;
        int tbh  = TBAR_H - 4;
        int tbh2 = tbh / 2;
        int tby0 = TBAR_Y + 2;
        uint32_t hi, lo1, lo2, tc;
        if (focused) {
            hi  = 0x6090E0; lo1 = 0x2858C0; lo2 = 0x3868D0; tc = 0xFFFFFF;
        } else if (is_min) {
            hi  = 0x405060; lo1 = 0x202C38; lo2 = 0x2C3848; tc = 0xA0B8D0;
        } else {
            hi  = 0x283848; lo1 = 0x101C28; lo2 = 0x182434; tc = 0x80A0C0;
        }
        vga_gradient(bx, tby0,      TBTN_W, tbh2,     hi,  lo1);
        vga_gradient(bx, tby0+tbh2, TBTN_W, tbh-tbh2, lo1, lo2);
        vga_hline(bx, tby0, TBTN_W, focused ? 0x88B8FF : (is_min ? 0x506070 : 0x384858));
        draw_border_3d(bx, tby0, TBTN_W, tbh, 1);
        /* Truncate title to fit in button (max 11 chars at 8px) */
        char lb[12];
        int k;
        for (k = 0; k < 11 && wp->title[k]; k++) lb[k] = wp->title[k];
        lb[k] = 0;
        vga_print_gfx(bx + 4, TBAR_Y + 6, lb, tc);
        bx += TBTN_W + TBTN_GAP;
    }

    /* Desktop watermark — subtle bottom-right corner branding */
    vga_print_gfx(SCR_W - 136, TBAR_Y - 14, "IronKernel v0.04", 0x0D1828);

    /* Windows (back to front).
       wm_z_top controls visual stacking (which window is on top), separate
       from wm_focused (keyboard).  This lets ELF windows appear in front
       when spawned without stealing keyboard from the shell. */
    int ztop = (wm_z_top >= 0 && wm_z_top < WM_MAX_WIN &&
                wm_wins[wm_z_top].alive) ? wm_z_top : wm_focused;
    for (int i = 0; i < WM_MAX_WIN; i++)
        if (i != ztop) draw_window(i);
    if (ztop >= 0) draw_window(ztop);

    /* Start menu popup (always on top of everything) */
    if (start_open)
        draw_start_menu();
}

/* ── Mouse cursor (11×18 arrow) ──────────────────────────────────── */
/*
 * With double-buffering the back buffer always holds the clean scene
 * (no cursor). Erasing the cursor = vga_blit_frame() to repaint the
 * clean back buffer onto the real screen. No pixel-save needed.
 */
static const uint8_t cursor_shape[18][11] = {
    {1,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,2,2,2,2,1},
    {1,2,2,2,2,2,2,1,1,1,0},
    {1,2,2,2,1,2,2,1,0,0,0},
    {1,2,2,1,0,1,2,2,1,0,0},
    {1,2,1,0,0,1,2,2,1,0,0},
    {1,1,0,0,0,0,1,2,2,1,0},
    {1,0,0,0,0,0,1,2,2,1,0},
    {0,0,0,0,0,0,0,1,1,0,0},
};
#define CUR_W 11
#define CUR_H 18

/* Draw cursor composited over back-buffer pixels.
 * Uses vga_cursor_blit which writes full rows (back-buf for transparent pixels)
 * so the cursor is never partially visible mid-write — reduces VGA tearing. */
static void cursor_draw(int cx, int cy)
{
    vga_cursor_blit(cx, cy, CUR_W, CUR_H,
                    &cursor_shape[0][0], C_CURSOR_OUT, C_CURSOR);
}

/* ── Mouse interaction ───────────────────────────────────────────── */
static uint8_t prev_btn = 0;
static int     drag_win  = -1;

/* Returns the window index that contains (px,py) in its title bar */
static int hit_titlebar(int px, int py)
{
    for (int i = WM_MAX_WIN-1; i >= 0; i--) {
        wm_win_t *wp = &wm_wins[i];
        if (!wp->alive) continue;
        if (px >= wp->x && px < wp->x + wp->w &&
            py >= wp->y && py < wp->y + TITLE_H) {
            return i;
        }
    }
    return -1;
}

/* Returns window index that contains point */
static int hit_window(int px, int py)
{
    /* Check from top (last drawn = focused = on top) down */
    /* Check focused first */
    if (wm_focused >= 0) {
        wm_win_t *wp = &wm_wins[wm_focused];
        if (wp->alive && px>=wp->x && px<wp->x+wp->w &&
            py>=wp->y && py<wp->y+wp->h) return wm_focused;
    }
    for (int i = WM_MAX_WIN-1; i >= 0; i--) {
        if (i == wm_focused) continue;
        wm_win_t *wp = &wm_wins[i];
        if (!wp->alive) continue;
        if (px>=wp->x && px<wp->x+wp->w && py>=wp->y && py<wp->y+wp->h)
            return i;
    }
    return -1;
}

/* Returns 1 if the WM should exit (shell window was closed). */
static int handle_mouse(void)
{
    uint8_t btn  = mouse_btn;
    int     mx   = mouse_x;
    int     my   = mouse_y;
    int     ldown = btn & 0x01;
    int     prev_ldown = prev_btn & 0x01;
    int     do_exit = 0;

    if (ldown && !prev_ldown) {
        /* ── Start menu popup: handle or close on any click ── */
        int popup_consumed = 0;
        if (start_open) {
            start_open = 0;  /* close on any click */
            int ph = popup_h();
            int py = TBAR_Y - ph;
            if (my >= py && my < TBAR_Y &&
                mx >= START_BTN_X && mx < START_BTN_X + POPUP_W) {
                popup_handle_click(my - py);
                popup_consumed = 1;
            } else if (my >= TBAR_Y && mx >= START_BTN_X &&
                       mx < START_BTN_X + START_BTN_W) {
                popup_consumed = 1;  /* click on start button → toggle off */
            }
        }

        if (!popup_consumed) {
        if (my >= TBAR_Y) {
            /* ── Start button ── */
            if (mx >= START_BTN_X && mx < START_BTN_X + START_BTN_W) {
                start_open = 1;
                load_start_elfs();
            } else {
            /* ── Taskbar window buttons: focus or restore minimized ── */
            int bx = TBTN_X0;
            for (int i = 0; i < WM_MAX_WIN; i++) {
                if (!wm_wins[i].alive) continue;
                if (bx + TBTN_W > SCR_W - 52) break;
                if (mx >= bx && mx < bx + TBTN_W) {
                    wm_wins[i].minimized = 0;
                    wm_focused = i;
                    wm_z_top   = i;
                    break;
                }
                bx += TBTN_W + TBTN_GAP;
            }
            }
        } else {
            int win = hit_window(mx, my);
            if (win >= 0) {
                wm_focused = win;
                wm_z_top   = win;
                wm_wins[win].dragging = 0;
                wm_win_t *wp = &wm_wins[win];

                /* Close button hit test */
                int cbx = wp->x + wp->w - CLOSE_SZ - 2;
                int cby = wp->y + 2;
                if (mx >= cbx && mx < cbx + CLOSE_SZ &&
                    my >= cby && my < cby + TITLE_H - 3) {
                    if (win == wm_shell_id) {
                        /* Close shell normally — reopen via Start > Open Shell */
                        wp->alive   = 0;
                        wm_shell_id = -1;
                        wm_focused  = -1;
                    } else {
                        wp->alive         = 0;
                        wm_focused        = (wm_shell_id >= 0) ? wm_shell_id : -1;
                        wm_z_top          = wm_focused;
                        elf_running       = 0;
                        wm_elf_gfx_active = 0;
                        wm_elf_out_win    = -1;  /* restore shell output routing */
                        /* Kill any task bound to this window so it can't
                           keep consuming keyboard after the window closes */
                        sched_lock();
                        {
                            task_t *tasks = sched_get_tasks();
                            for (int t = 1; t < MAX_TASKS; t++) {
                                if ((tasks[t].state == TASK_READY ||
                                     tasks[t].state == TASK_RUNNING) &&
                                    tasks[t].win_id == win)
                                    tasks[t].state = TASK_DEAD;
                            }
                        }
                        sched_unlock();
                    }
                } else {
                    /* Minimize button hit test (all windows) */
                    int mbx = wp->x + wp->w - 2 * CLOSE_SZ - 4;
                    if (mx >= mbx && mx < mbx + CLOSE_SZ &&
                        my >= cby && my < cby + TITLE_H - 3) {
                        wp->minimized = 1;
                        wm_focused    = (win == wm_shell_id) ? -1 :
                                        ((wm_shell_id >= 0) ? wm_shell_id : -1);
                        wm_z_top      = wm_focused;
                    } else if (my >= wp->y && my < wp->y + TITLE_H) {
                        drag_win     = win;
                        wp->drag_ox  = mx - wp->x;
                        wp->drag_oy  = my - wp->y;
                        wp->dragging = 1;
                    }
                }
            } else {
                /* Click on desktop background → defocus all + close menu */
                wm_focused = -1;
            }
        }
        } /* !popup_consumed */
    }

    if (!ldown) {
        if (drag_win >= 0) wm_wins[drag_win].dragging = 0;
        drag_win = -1;
    }

    if (ldown && drag_win >= 0) {
        wm_win_t *wp = &wm_wins[drag_win];
        int nx = mx - wp->drag_ox;
        int ny = my - wp->drag_oy;
        /* Allow windows to phase through left/right/bottom edges but
           ensure at least 30 px remains visible so the window can still
           be grabbed and can never completely disappear off screen.
           The title bar top is clamped to y=0 so you can always grab it. */
        int min_vis = 30;
        if (nx < -(wp->w - min_vis)) nx = -(wp->w - min_vis);
        if (nx >  SCR_W - min_vis)   nx =  SCR_W - min_vis;
        if (ny < 0)                  ny = 0;
        if (ny >  TBAR_Y - TITLE_H)  ny =  TBAR_Y - TITLE_H;
        wp->x = nx; wp->y = ny;
    }

    prev_btn = btn;
    return do_exit;
}

/* ── Splash screen ────────────────────────────────────────────────── */

static void delay_ticks(uint32_t t)
{
    uint32_t s = pit_get_ticks();
    while (pit_get_ticks() - s < t)
        __asm__ volatile("pause");
}

/* ── Alien splash drawing primitives ─────────────────────────────── */

/* Filled ellipse via horizontal scanlines */
static void fill_ellipse(int cx, int cy, int rx, int ry, uint32_t color)
{
    if (rx <= 0 || ry <= 0) return;
    int rx2 = rx * rx, ry2 = ry * ry;
    for (int dy = -ry; dy <= ry; dy++) {
        int n = ry2 - dy * dy;
        if (n < 0) continue;
        int xw = 0;
        while ((xw + 1) * (xw + 1) * ry2 <= rx2 * n) xw++;
        vga_hline(cx - xw, cy + dy, 2 * xw + 1, color);
    }
}

static void fill_circle(int cx, int cy, int r, uint32_t color)
{
    fill_ellipse(cx, cy, r, r, color);
}

/* Thick line: stamp circles along the path */
static void spl_line(int x0, int y0, int x1, int y1, int t, uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx);
    int sy    = (dy < 0 ? -dy : dy);
    if (sy > steps) steps = sy;
    if (!steps) { fill_circle(x0, y0, t, color); return; }
    for (int i = 0; i <= steps; i++)
        fill_circle(x0 + dx * i / steps, y0 + dy * i / steps, t, color);
}

/* Draw the alien splash and transition directly to desktop (no keypress) */
static void wm_splash(void)
{
/* ── Alien color palette ── */
#define AG   0x2CA840   /* body green — main           */
#define AGD  0x176428   /* dark seam / shadow green    */
#define AGL  0x44C85C   /* light highlight green       */

    /* ── Background: deep space ── */
    vga_gradient(0, 0, SCR_W, SCR_H, 0x080810, 0x020206);

    /* ── Legs (draw first — bottom layer) ── */
    fill_ellipse(370, 460, 33, 52, AG);
    fill_ellipse(430, 460, 33, 52, AG);
    /* leg inner seam lines */
    for (int y = 418; y < 508; y++) vga_pixel(370, y, AGD);
    for (int y = 418; y < 508; y++) vga_pixel(430, y, AGD);
    delay_ticks(22);

    /* ── Body ── */
    fill_ellipse(400, 375, 78, 82, AG);
    /* ── Arms ── */
    fill_ellipse(307, 358, 55, 27, AG);
    fill_ellipse(493, 358, 55, 27, AG);
    /* ── Neck fill (bridge between head and body) ── */
    fill_ellipse(400, 302, 40, 20, AG);
    /* Belt / body horizontal seam */
    for (int x = 334; x < 466; x++) vga_pixel(x, 348, AGD);
    /* Vertical body center seam */
    for (int y = 308; y < 452; y++) vga_pixel(400, y, AGD);
    delay_ticks(22);

    /* ── Head ── */
    fill_circle(400, 208, 96, AG);
    /* Head shading: slightly darker right-lower area */
    fill_ellipse(435, 234, 58, 52, 0x228538);
    /* Re-center: main green on upper-left of head */
    fill_ellipse(384, 192, 62, 58, AG);
    /* Soften the shading boundary */
    fill_ellipse(400, 208, 45, 42, AG);
    /* Head vertical center seam */
    for (int y = 118; y < 300; y++) vga_pixel(400, y, AGD);
    /* Head highlight: small bright patch top-left */
    fill_ellipse(370, 155, 22, 18, AGL);
    delay_ticks(22);

    /* ── Antenna stems ── */
    spl_line(384, 122, 343, 65, 7, AG);
    spl_line(416, 117, 458, 62, 7, AG);
    delay_ticks(15);

    /* ── Antenna tips ── */
    fill_circle(336, 56, 19, 0xD8D8E8);   /* left  — silver-white */
    fill_circle(465, 53, 19, 0x1A2EC0);   /* right — blue         */
    fill_circle(330, 50, 8,  0xF4F4FC);   /* left  inner bright   */
    fill_circle(472, 47, 7,  0x4462E8);   /* right inner bright   */
    delay_ticks(15);

    /* ── Eyes (large black almond shapes) ── */
    fill_ellipse(358, 223, 55, 47, 0x060810);
    fill_ellipse(442, 223, 55, 47, 0x060810);
    delay_ticks(15);

    /* ── Eye highlights ── */
    fill_circle(340, 217, 13, 0xEEEEF4);   /* left — large white     */
    fill_circle(360, 243,  8, 0xEEEEF4);   /* left — small white     */
    fill_circle(456, 219, 12, 0xA4B0BE);   /* right — silver         */
    fill_circle(448, 236,  5, 0xC8D0D8);   /* right — small silver   */
    delay_ticks(10);

    /* ── Mouth ── */
    fill_ellipse(400, 272, 8, 7, 0x0E0808);
    delay_ticks(10);

    /* ── Hand/foot accent patches ── */
    fill_circle(257, 360, 9, 0xEEEEF4);   /* left hand  — white */
    fill_circle(456, 508, 8, 0x1A2EC0);   /* right foot — blue  */

    /* ── Text ── */
    int tx1 = (SCR_W - 10 * 8) / 2;
    vga_print_gfx(tx1 + 1, 531, "IronKernel", 0x000000);   /* shadow */
    vga_print_gfx(tx1,     530, "IronKernel", 0xEEEEFF);
    vga_print_gfx((SCR_W - 19 * 8) / 2, 543,
                  "GUI  v0.04   x86-64", 0x5577A0);
    delay_ticks(30);

    /* ── Loading bar ── */
    {
        int bx = (SCR_W - 220) / 2, by = 558;
        vga_rect(bx,     by,     220, 12, 0x101020);
        vga_hline(bx,    by,     220,     0x28364E);
        vga_hline(bx,    by + 11, 220,    0x28364E);
        vga_vline(bx,    by,      12,     0x28364E);
        vga_vline(bx+219,by,      12,     0x28364E);
        for (int i = 0; i < 216; i += 3) {
            /* gradient fill: left part teal, right bright blue */
            uint32_t lc = (i < 108) ? 0x22AAFF : 0x4488FF;
            vga_rect(bx + 2, by + 2, i, 8, lc);
            delay_ticks(2);
        }
    }
    delay_ticks(50);

    /* ── Wipe to desktop (top-to-bottom) ── */
    for (int row = 0; row < SCR_H; row += 4) {
        vga_rect(0, row, SCR_W, 4, C_DESKTOP);
        delay_ticks(1);
    }

    /* Drain any keys that accumulated during the animation */
    while (keyboard_getchar()) {}

#undef AG
#undef AGD
#undef AGL
}

/* ── Windowed ELF execution ──────────────────────────────────────── */

/* Kernel-task entry point for a spawned ELF.
   Reads win_id / elf_name / elf_ext from its own task_t,
   runs the ELF, then leaves the window open for the user to read. */
static void elf_task_trampoline(void)
{
    int id  = sched_current_id();
    task_t *me = sched_get_tasks() + id;
    int win = me->win_id;

    /* Startup banner */
    if (win >= 0 && win < WM_MAX_WIN && wm_wins[win].alive) {
        wm_puts_col(win, " Running: ", 0x22DDFF);
        for (int k = 0; k < 8 && me->elf_name[k] && me->elf_name[k] != ' '; k++) {
            char tmp[2] = {me->elf_name[k], 0};
            wm_puts_col(win, tmp, 0x22DDFF);
        }
        wm_puts_col(win, ".", 0x22DDFF);
        for (int k = 0; k < 3 && me->elf_ext[k] && me->elf_ext[k] != ' '; k++) {
            char tmp[2] = {me->elf_ext[k], 0};
            wm_puts_col(win, tmp, 0x22DDFF);
        }
        wm_puts_col(win, "\n\n", 0xC8C8D8);
        wm_wins[win].dirty = 1;
    }

    /* Direct all vga_print output to this window for the duration of elf_exec */
    wm_elf_out_win = win;

    /* Execute ELF — blocks until SYS_EXIT; scheduler still runs WM task */
    int rc = elf_exec(me->elf_name, me->elf_ext);

    /* Debug: confirm trampoline reached this point */
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)'['));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)'W'));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)'M'));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)']'));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)' '));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)'t'));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)'r'));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)'a'));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)'m'));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)'p'));
    __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)'\n'));
    int was_gfx    = wm_elf_gfx_active; /* save before clearing */
    elf_running       = 0;
    wm_elf_out_win    = -1;  /* restore shell routing */
    wm_elf_gfx_active = 0;

    if (win >= 0 && win < WM_MAX_WIN && wm_wins[win].alive) {
        if (was_gfx) {
            /* GFX ELF: auto-close window to free the slot immediately.
               No text footer — pixel UI apps don't need one. */
            wm_wins[win].alive = 0;
            if (wm_focused == win)
                wm_focused = (wm_shell_id >= 0) ? wm_shell_id : -1;
            if (wm_z_top == win)
                wm_z_top = wm_focused;
        } else {
            /* Text ELF: show footer so user can read the output. */
            if (rc != 0)
                wm_puts_col(win, " [ELF load failed — check filename]\n", 0xFF5555);
            else
                wm_puts_col(win, "\n [Process exited. Click X to close.]\n", 0x888898);
            wm_wins[win].dirty = 1;
            /* Return keyboard focus to shell — window stays open for reading
               but the user shouldn't have to click shell to keep typing. */
            if (wm_focused == win && wm_shell_id >= 0)
                wm_focused = wm_shell_id;
        }
    }

    task_exit();
}

/* Parse "exec FILENAME.ELF", create a window, and spawn a kernel task.
   The task runs elf_task_trampoline which calls elf_exec internally. */
void wm_spawn_elf(const char *cmd)
{
    /* Skip "exec" keyword then spaces */
    while (*cmd && *cmd != ' ') cmd++;
    while (*cmd == ' ')         cmd++;

    if (!*cmd) {
        wm_puts_col(wm_shell_id, " Usage: exec FILE.ELF\n", 0xFF5555);
        return;
    }
    if (elf_running) {
        wm_puts_col(wm_shell_id,
            " A process is already running. Wait for it to finish.\n", 0xFF5555);
        return;
    }

    /* Parse to FAT 8.3 uppercase space-padded format */
    char name[9], ext[4];
    for (int i = 0; i < 8; i++) name[i] = ' '; name[8] = 0;
    for (int i = 0; i < 3; i++) ext[i]  = ' '; ext[3]  = 0;
    int i = 0;
    while (*cmd && *cmd != '.' && *cmd != ' ' && i < 8) {
        char c = *cmd++;
        name[i++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    if (*cmd == '.') cmd++;
    int j = 0;
    while (*cmd && *cmd != ' ' && j < 3) {
        char c = *cmd++;
        ext[j++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }

    /* Parse optional argument after the ELF name (e.g. "README.TXT") */
    while (*cmd == ' ') cmd++;
    char elf_arg[32];
    int ak = 0;
    while (*cmd && ak < 31) elf_arg[ak++] = *cmd++;
    elf_arg[ak] = 0;

    /* Build display title "NAME.EXT" */
    char title[13];
    int ti = 0;
    for (int k = 0; k < 8 && name[k] != ' '; k++) title[ti++] = name[k];
    title[ti++] = '.';
    for (int k = 0; k < 3 && ext[k] != ' '; k++) title[ti++] = ext[k];
    title[ti] = 0;

    /* Cascade window position */
    static int cas_x = 20, cas_y = 20;
    int win = wm_create(cas_x, cas_y, 554, 546, title);
    cas_x += 20; if (cas_x > 120) cas_x = 20;
    cas_y += 20; if (cas_y > 120) cas_y = 20;

    if (win < 0) {
        wm_puts_col(wm_shell_id, " No window slots available.\n", 0xFF5555);
        return;
    }

    wm_z_top    = win;   /* bring ELF window to visual front */
    wm_focused  = win;   /* give keyboard focus to the ELF immediately */
    elf_running = 1;

    /* Spawn task; lock scheduler so fields are set before it can run */
    sched_lock();
    int tid = task_spawn(title, elf_task_trampoline);
    if (tid >= 0) {
        task_t *tsk = sched_get_tasks() + tid;
        tsk->win_id = win;
        for (int k = 0; k < 9; k++) tsk->elf_name[k] = name[k];
        for (int k = 0; k < 4; k++) tsk->elf_ext[k]  = ext[k];
        for (int k = 0; k < 32; k++) tsk->elf_arg[k]  = elf_arg[k];
    } else {
        elf_running       = 0;
        wm_wins[win].alive = 0;
        wm_puts_col(wm_shell_id, " Could not spawn task (MAX_TASKS reached).\n", 0xFF5555);
    }
    sched_unlock();
}

/* ── Build prompt string ("IK@ironkernel:/PATH> ") ──────────────── */
static void print_prompt(int win)
{
    wm_cur_fg = 0x22CCFF;  /* bright cyan */
    wm_puts(win, "IK@ironkernel:");
    wm_cur_fg = 0xFFFFFF;
    wm_puts(win, fat32_cwd);
    wm_puts(win, "> ");
    wm_cur_fg = C_WIN_TXT;
}

/* ── Fullscreen command detection ────────────────────────────────── */

/* ── Shell history ────────────────────────────────────────────────── */
#define HIST_MAX 16
static char hist_buf[HIST_MAX][256];
static int  hist_n   = 0;   /* number of saved entries               */
static int  hist_i   = -1;  /* browsing index (-1 = live input)      */
static char hist_draft[256]; /* live input saved while browsing       */

/* Erase disp_len chars on screen, retype buf[0..len-1],
   then move visual cursor left (ilen-ipos) steps so it sits at ipos. */
static void wm_redraw_input(int win, const char *buf, int len,
                             int ipos, int disp_len)
{
    for (int i = 0; i < disp_len; i++) wm_putchar(win, '\b');
    for (int i = 0; i < len;      i++) wm_putchar(win, buf[i]);
    for (int i = ipos; i < len;   i++) wm_putchar(win, '\x01');
}

/* ── wm_run — main entry point ────────────────────────────────────── */
void wm_run(void)
{
    wm_is_running = 1;
    /* Don't install hooks yet — splash uses direct framebuffer drawing */
    mouse_init();
    wm_splash();

    /* Install hooks now that windows are about to be created */
    vga_print_hook = wm_hook_print;
    vga_color_hook = wm_hook_color;

    /* ── Create windows ── */
    int shell_id = wm_create(6,   28, 554, 546, "IronKernel Shell");
    int info_id  = wm_create(566, 28, 228, 252, "System Info");
    int demo_id  = wm_create(566, 284, 228, 290, "Color Demo");
    wm_focused   = shell_id;
    wm_z_top     = shell_id;
    wm_shell_id  = shell_id;   /* hook target — never changes */
    wm_demo_id   = demo_id;

    /* ── Populate info window ── */
    wm_cur_fg = 0x22DDFF;   /* cyan for IronKernel heading */
    wm_puts(info_id, " IronKernel v0.04\n");
    wm_cur_fg = C_WIN_TXT;
    wm_puts(info_id, " Arch  : x86-64\n");
    wm_puts(info_id, " Mode  : VBE 800x600x32\n");
    wm_puts(info_id, " Disk  : FAT32\n");
    wm_puts(info_id, " Mouse : PS/2\n");
    wm_cur_fg = C_WIN_TXT;

    /* ── Initial prompt in shell window ── */
    wm_cur_fg = C_WIN_TXT;
    wm_puts(shell_id, " IronKernel GUI Shell\n");
    wm_puts(shell_id, " Type 'help' for commands.\n");
    wm_puts(shell_id, " Type 'exit' to return to text shell.\n\n");
    wm_cur_fg = C_WIN_TXT;
    print_prompt(shell_id);

    /* ── Input buffer ── */
    static char input[256];
    int ilen     = 0;
    int ipos     = 0;   /* cursor position within input (0..ilen) */
    int disp_len = 0;   /* chars currently displayed for this line */

    /* Startup sound — plays BOOT.WAV from FAT32 if present,
       synthesised AC97 chord if not, PC speaker as last resort. */
    if (ac97_detected())
        ac97_play_boot_wav();
    else
        speaker_startup_chime();

    prev_btn  = 0;  drag_win  = -1;

    /* Force a full redraw on first frame */
    int full_dirty   = 1;
    int clock_dirty  = 0;
    int cursor_dirty = 0;
    int wm_exit      = 0;
    int pending_full = 1;          /* full redraw queued but maybe rate-limited */
    uint32_t last_clock_sec  = 0; /* for clock-based redraws                   */
    uint32_t last_full_tick  = 0; /* for frame-rate cap                        */
    int prev_elf_running = 0;      /* detect ELF exit to drain keyboard */
    int prev_focused     = shell_id; /* detect focus change to drain keyboard */
    int prev_start_open  = 0;      /* detect start menu toggle → full redraw */
    int last_cursor_x    = -CUR_W; /* cursor position from last render (for erase) */
    int last_cursor_y    = -CUR_H;

    /* ── Main loop ── */
    while (!wm_exit) {
        full_dirty   = 0;
        clock_dirty  = 0;
        cursor_dirty = 0;

        /* ── Power actions (set by start menu popup) ── */
        if (wm_power_action) {
            int act = wm_power_action;
            wm_power_action = 0;
            if (act == 1) { wm_exit = 1; break; }   /* exit immediately */
            if (act == 2) { shell_dispatch("shutdown"); }
            if (act == 3) { shell_dispatch("reboot");  }
        }

        /* ── Clock tick: update taskbar time cheaply once per second ── */
        {
            uint32_t cur_sec = pit_get_ticks() / 100;
            if (cur_sec != last_clock_sec) {
                last_clock_sec = cur_sec;
                clock_dirty = 1;   /* partial update only — no full redraw */
            }
        }

        /* ── Mouse ── */
        mouse_poll();
        {
            /* Save window positions before handle_mouse to detect drag moves */
            int save_x[WM_MAX_WIN], save_y[WM_MAX_WIN];
            int prev_focused = wm_focused;
            int prev_z_top   = wm_z_top;
            for (int i = 0; i < WM_MAX_WIN; i++) {
                save_x[i] = wm_wins[i].x;
                save_y[i] = wm_wins[i].y;
            }
            int save_alive[WM_MAX_WIN];
            for (int i = 0; i < WM_MAX_WIN; i++)
                save_alive[i] = wm_wins[i].alive;

            if (handle_mouse()) wm_exit = 1;
            if (wm_focused != prev_focused) full_dirty = 1;
            if (wm_z_top   != prev_z_top)   full_dirty = 1;
            if (start_open != prev_start_open) full_dirty = 1;
            prev_start_open = start_open;
            for (int i = 0; i < WM_MAX_WIN; i++) {
                if (wm_wins[i].x != save_x[i] || wm_wins[i].y != save_y[i])
                    full_dirty = 1;
                if (wm_wins[i].alive != save_alive[i])
                    full_dirty = 1;
            }
        }

        if (mouse_moved) cursor_dirty = 1;

        /* ── Keyboard ── */
        /* Drain keyboard buffer when:
           - ELF just finished (prevent chars typed during ELF replaying into shell)
           - Focus just moved TO the shell (discard chars typed in ELF window)
           - Focus just moved FROM shell TO an ELF window (discard shell input) */
        if ((prev_elf_running && !elf_running) ||
            (prev_focused != wm_shell_id && wm_focused == wm_shell_id) ||
            (prev_focused == wm_shell_id && wm_focused != wm_shell_id)) {
            while (keyboard_getchar()) {}
        }
        prev_elf_running = elf_running;
        prev_focused     = wm_focused;

        /* ── Scroll focused text window (Up/Down arrow) ─────────────────
           Uses edge flags set by the IRQ independently of the ring buffer.
           Skipped when a GFX ELF has focus — GFX ELFs handle their own
           scrolling internally via SYS_READ_RAW, and firing scroll on a
           pixel-mode window causes spurious full redraws with no effect. */
        int focused_is_gfx = wm_elf_gfx_active &&
                              wm_elf_out_win >= 0 &&
                              wm_focused == wm_elf_out_win;
        if (kb_scroll_up) {
            kb_scroll_up = 0;
            if (!focused_is_gfx &&
                wm_focused >= 0 && wm_wins[wm_focused].alive &&
                !wm_wins[wm_focused].minimized) {
                wm_win_t *sw = &wm_wins[wm_focused];
                sw->top_row -= 3;
                if (sw->top_row < 0) sw->top_row = 0;
                full_dirty = 1;
            }
        }
        if (kb_scroll_dn) {
            kb_scroll_dn = 0;
            if (!focused_is_gfx &&
                wm_focused >= 0 && wm_wins[wm_focused].alive &&
                !wm_wins[wm_focused].minimized) {
                wm_win_t *sw = &wm_wins[wm_focused];
                int max_top = sw->cur_row - sw->vis_rows + 1;
                if (max_top < 0) max_top = 0;
                if (sw->top_row < max_top) {
                    sw->top_row += 3;
                    if (sw->top_row > max_top) sw->top_row = max_top;
                }
                full_dirty = 1;
            }
        }

        /* WM consumes keyboard only when the shell window is focused.
           When an ELF window is focused, SYS_READ_RAW is the sole consumer. */
        char ch = (wm_focused == wm_shell_id) ? keyboard_getchar() : 0;

        /* Alt+Tab — cycle window focus (consume regardless of focused window) */
        if (ch == '\t' && keyboard_alt()) {
            int start = (wm_focused < 0) ? 0 : wm_focused;
            int next  = (start + 1) % WM_MAX_WIN;
            while (next != start) {
                if (wm_wins[next].alive && !wm_wins[next].minimized) {
                    wm_focused = next;
                    full_dirty = 1;
                    break;
                }
                next = (next + 1) % WM_MAX_WIN;
            }
            ch = 0;
        }

        if (ch && wm_focused == wm_shell_id) {
            /* Keys are only accepted when the shell window is focused.
               Click the shell window title bar or client area to focus it.
               This prevents accidental typing when the info window or
               desktop background has focus. */
            full_dirty = 1;

            unsigned char uch = (unsigned char)ch;

            if (uch == KEY_LEFT) {
                /* Left arrow = previous command (older history) */
                if (hist_n > 0) {
                    if (hist_i == -1) {
                        for (int i = 0; i <= ilen; i++) hist_draft[i] = input[i];
                        hist_i = 0;
                    } else if (hist_i < hist_n - 1) {
                        hist_i++;
                    }
                    int hidx = hist_n - 1 - hist_i;
                    int nlen = 0;
                    while (hist_buf[hidx][nlen]) nlen++;
                    wm_redraw_input(wm_shell_id, hist_buf[hidx], nlen, nlen, disp_len);
                    for (int i = 0; i < nlen; i++) input[i] = hist_buf[hidx][i];
                    input[nlen] = '\0';
                    ilen = nlen; ipos = nlen; disp_len = nlen;
                }
            } else if (uch == KEY_RIGHT) {
                /* Right arrow = next command (newer history / restore draft) */
                if (hist_i > 0) {
                    hist_i--;
                    int hidx = hist_n - 1 - hist_i;
                    int nlen = 0;
                    while (hist_buf[hidx][nlen]) nlen++;
                    wm_redraw_input(wm_shell_id, hist_buf[hidx], nlen, nlen, disp_len);
                    for (int i = 0; i < nlen; i++) input[i] = hist_buf[hidx][i];
                    input[nlen] = '\0';
                    ilen = nlen; ipos = nlen; disp_len = nlen;
                } else if (hist_i == 0) {
                    hist_i = -1;
                    int dlen = 0;
                    while (hist_draft[dlen]) dlen++;
                    wm_redraw_input(wm_shell_id, hist_draft, dlen, dlen, disp_len);
                    for (int i = 0; i <= dlen; i++) input[i] = hist_draft[i];
                    ilen = dlen; ipos = dlen; disp_len = dlen;
                }
            } else if (uch == KEY_UP || uch == KEY_DOWN) {
                /* UP/DOWN already handled as window scroll above — ignore here */
            } else if (ch == '\n') {
                wm_putchar(wm_shell_id, '\n');
                input[ilen] = '\0';
                /* Save non-empty commands to history */
                if (ilen > 0) {
                    if (hist_n < HIST_MAX) {
                        for (int i = 0; i <= ilen; i++) hist_buf[hist_n][i] = input[i];
                        hist_n++;
                    } else {
                        for (int h = 0; h < HIST_MAX - 1; h++)
                            for (int c = 0; c < 256; c++) hist_buf[h][c] = hist_buf[h+1][c];
                        for (int i = 0; i <= ilen; i++) hist_buf[HIST_MAX-1][i] = input[i];
                    }
                    hist_i = -1;
                }
                ilen = 0; ipos = 0; disp_len = 0;

                /* "exit" → leave WM */
                if (input[0]=='e' && input[1]=='x' && input[2]=='i' &&
                    input[3]=='t' && input[4]=='\0') {
                    wm_exit = 1;
                } else if (input[0]) {
                    /* Block commands that write directly to the framebuffer or
                       run user-mode code — they steal the screen and block the
                       WM loop, causing display corruption.
                       These work fine in the text shell (type 'exit' to go back). */
                    /* exec <elf> — launch in its own window (non-blocking) */
                    int is_exec = (input[0]=='e'&&input[1]=='x'&&input[2]=='e'&&
                                   input[3]=='c'&&(input[4]=='\0'||input[4]==' '));
                    /* Commands that take over the display or re-enter the WM */
                    int blocked = 0;
                    if (input[0]=='g'&&input[1]=='f'&&input[2]=='x'&&
                        (input[3]=='\0'||input[3]==' ')) blocked = 1;
                    if (input[0]=='e'&&input[1]=='d'&&input[2]=='i'&&input[3]=='t'&&
                        (input[4]=='\0'||input[4]==' ')) blocked = 1;
                    /* gui — already in the GUI, re-entry makes no sense */
                    if (input[0]=='g'&&input[1]=='u'&&input[2]=='i'&&
                        (input[3]=='\0'||input[3]==' ')) blocked = 1;

                    if (is_exec) {
                        wm_spawn_elf(input);
                        full_dirty = 1;
                    } else if (blocked) {
                        wm_cur_fg = 0xFF4444;  /* red */
                        wm_puts(wm_shell_id,
                            " Not available in GUI shell.\n"
                            " Type 'exit' to return to text shell.\n");
                        wm_cur_fg = C_WIN_TXT;
                    } else if (input[0]=='c' && input[1]=='l' && input[2]=='e' &&
                               input[3]=='a' && input[4]=='r' && input[5]=='\0') {
                        wm_clear_win(wm_shell_id);
                        full_dirty = 1;
                    } else {
                        /* Spawn shell command as a background task so the
                           WM loop (mouse, redraws) stays live during execution. */
                        if (wm_cmd_running) {
                            wm_cur_fg = 0xFF4444;
                            wm_puts(wm_shell_id, " Command already running.\n");
                            wm_cur_fg = C_WIN_TXT;
                        } else {
                            int k;
                            for (k = 0; input[k] && k < 255; k++)
                                wm_cmd_buf[k] = input[k];
                            wm_cmd_buf[k] = '\0';
                            wm_cmd_running = 1;
                            sched_lock();
                            int ctid = task_spawn("wm_cmd", wm_shell_cmd_task);
                            if (ctid < 0) {
                                /* No task slots — fall back to inline execution */
                                wm_cmd_running = 0;
                                shell_dispatch(input);
                            }
                            sched_unlock();
                        }
                        full_dirty = 1;
                    }
                }

                /* Print prompt immediately for instant commands;
                   for background tasks the prompt is printed below
                   when wm_cmd_running drops to 0. */
                if (!wm_exit && !wm_cmd_running) {
                    wm_putchar(wm_shell_id, '\n');
                    print_prompt(wm_shell_id);
                }
            } else if (ch == '\b') {
                if (ipos > 0) {
                    /* Remove char at ipos-1, shift remainder left */
                    for (int i = ipos - 1; i < ilen - 1; i++) input[i] = input[i+1];
                    ilen--; ipos--;
                    wm_redraw_input(wm_shell_id, input, ilen, ipos, disp_len);
                    disp_len = ilen;
                }
            } else if (uch >= 0x20 && uch < 0x80 && ilen < 254) {
                /* Insert char at ipos */
                for (int i = ilen; i > ipos; i--) input[i] = input[i-1];
                input[ipos] = ch;
                ilen++; ipos++;
                if (ipos == ilen) {
                    /* Cursor at end — fast path: just append */
                    wm_putchar(wm_shell_id, ch);
                    disp_len++;
                } else {
                    wm_redraw_input(wm_shell_id, input, ilen, ipos, disp_len);
                    disp_len = ilen;
                }
            }
        }

        /* ── Dirty-window poll: ELF tasks write to wm_wins[].dirty ──
           When any window has been written to by a background task,
           trigger a full redraw so the new text appears on screen. */
        for (int i = 0; i < WM_MAX_WIN; i++) {
            if (wm_wins[i].alive && wm_wins[i].dirty) {
                wm_wins[i].dirty = 0;
                full_dirty = 1;
            }
        }

        /* Accumulate full_dirty into pending_full so the frame cap
           doesn't silently drop redraws — it just defers them. */
        if (full_dirty) pending_full = 1;

        /* ── Render ──────────────────────────────────────────────────
         * Back buffer always holds the clean scene (no cursor).
         *
         * pending_full → re-render scene into back buffer, blit, draw cursor.
         *   Capped at ~50 fps (2 PIT ticks) so rapid mouse drags and fast
         *   key repeat don't saturate the CPU with 800×600 redraws.
         * cursor_dirty → re-blit existing back buffer (erases old cursor),
         *                then draw cursor at new position.
         * Neither path writes to the screen mid-render → zero flicker.
         * ─────────────────────────────────────────────────────────── */
        uint32_t now_tick = pit_get_ticks();
        if (pending_full && (now_tick - last_full_tick >= 2)) {
            pending_full = 0;
            last_full_tick = now_tick;
            if (wm_elf_gfx_active && drag_win < 0 && wm_elf_out_win >= 0) {
                /* ELF fast path: patch new ELF pixels into back buffer, then
                   blit the full frame with cursor composited inline.
                   We blit 800×600 rather than just the ELF rect so the cursor
                   is composited in the same pass — eliminating the brief
                   cursor-absent window that existed between the old partial
                   blit and the subsequent cursor_draw call. */
                wm_win_t *ew = &wm_wins[wm_elf_out_win];
                int ecx_cl = ew->x + BORDER;
                int ecy_cl = ew->y + TITLE_H + BORDER;
                vga_begin_frame();
                vga_blit_pixels(ecx_cl, ecy_cl, wm_elf_gfx_cw, wm_elf_gfx_ch,
                                wm_elf_gfx_buf, wm_elf_gfx_cw);
                int ecx = mouse_x, ecy = mouse_y;
                vga_end_frame_cursor(ecx, ecy, CUR_W, CUR_H,
                                     &cursor_shape[0][0], C_CURSOR_OUT, C_CURSOR);
                last_cursor_x = ecx; last_cursor_y = ecy;
            } else {
                vga_begin_frame();
                draw_desktop();
                /* Snapshot cursor position immediately before the blit.
                   vga_end_frame_cursor composites the cursor inline as each
                   scan row is written to VRAM — the cursor is never absent
                   on screen, even during the full 800×600 blit. */
                int dcx = mouse_x, dcy = mouse_y;
                vga_end_frame_cursor(dcx, dcy, CUR_W, CUR_H,
                                     &cursor_shape[0][0], C_CURSOR_OUT, C_CURSOR);
                last_cursor_x = dcx;
                last_cursor_y = dcy;
            }
        } else if (!pending_full && clock_dirty) {
            /* Only the taskbar clock changed — update just that strip in the
               back buffer and blit those rows to screen.  Skips full scene
               redraw (800×600 clear + all windows) which was the main cause
               of "gets slow over time" from the per-second full redraw.
               Cursor erase/redraw is deferred to after end_frame_partial and
               wrapped in sched_lock so the cursor is never absent for more
               than a few microseconds — moving the erase to the top (before
               the clock render) left a ~200μs cursor-absent window that
               included possible 10ms task-switch gaps, causing the cursor to
               visibly vanish once per second, especially noticeable during
               fast mouse movement. */
            uint32_t ticks = pit_get_ticks();
            uint32_t secs  = (ticks / 100) % 60;
            uint32_t mins  = (ticks / 100) / 60;
            char tbuf[8];
            tbuf[0] = '0' + (char)((mins / 10) % 10);
            tbuf[1] = '0' + (char)(mins % 10);
            tbuf[2] = ':';
            tbuf[3] = '0' + (char)(secs / 10);
            tbuf[4] = '0' + (char)(secs % 10);
            tbuf[5] = '\0';
            vga_begin_frame();
            /* Repaint the clock strip of the taskbar gradient */
            vga_gradient(SCR_W-60, TBAR_Y,          60, TBAR_H/2,          0x182840, 0x0C1828);
            vga_gradient(SCR_W-60, TBAR_Y+TBAR_H/2, 60, TBAR_H-TBAR_H/2,  0x0A1420, 0x141E30);
            vga_hline(SCR_W-60, TBAR_Y, 60, 0x4878B8);
            /* Separator + framed clock */
            vga_vline(SCR_W-56, TBAR_Y+2, TBAR_H-4, 0x1A2840);
            vga_vline(SCR_W-55, TBAR_Y+2, TBAR_H-4, 0x304858);
            {
                int ck_x = SCR_W-50, ck_y = TBAR_Y+2, ck_w = 46, ck_h = TBAR_H-4;
                vga_gradient(ck_x, ck_y, ck_w, ck_h, 0x0C1828, 0x182438);
                draw_border_3d(ck_x, ck_y, ck_w, ck_h, 0);
                vga_print_gfx(ck_x+5, TBAR_Y+6, tbuf, C_TASKBAR_TXT);
            }
            /* Re-draw any windows that overlap the taskbar rows so their
               content sits on top of the clock gradient in the back buffer. */
            for (int i = 0; i < WM_MAX_WIN; i++) {
                if (wm_wins[i].alive && !wm_wins[i].minimized &&
                    wm_wins[i].y + wm_wins[i].h > TBAR_Y)
                    draw_window(i);
            }
            vga_end_frame_partial(TBAR_Y, TBAR_H);   /* blit 800×20 rows only */
            /* Atomically erase old cursor then draw at new position.
               Both operations inside sched_lock so the cursor is absent
               for < 10μs — no task-switch gap can land between them.
               The partial blit above only covers rows 580..599; any cursor
               pixels outside that band are still on screen at old position,
               which is correct — we replace them atomically here. */
            int cmx = mouse_x, cmy = mouse_y;
            sched_lock();
            vga_backbuf_to_screen_rect(last_cursor_x, last_cursor_y, CUR_W, CUR_H);
            cursor_draw(cmx, cmy);
            sched_unlock();
            last_cursor_x = cmx; last_cursor_y = cmy;
        } else if (cursor_dirty) {
            /* Erase old cursor first by restoring clean back-buffer pixels,
               then draw new cursor.  This order is critical: the old
               draw-then-erase order partially destroyed the new cursor when
               old and new rects overlapped, causing visible cursor holes at
               high mouse speed.  Since vga_cursor_blit composites against
               the clean back buffer, drawing after erase is always correct
               regardless of overlap.
               sched_lock prevents the PIT-driven task switch from firing
               between the erase and draw — otherwise the cursor is absent
               on screen for a full scheduler quantum (~10ms), which is
               visually noticeable especially during fast mouse movement
               where path C fires hundreds of times per second. */
            int cx = mouse_x, cy = mouse_y;
            sched_lock();
            vga_backbuf_to_screen_rect(last_cursor_x, last_cursor_y, CUR_W, CUR_H);
            cursor_draw(cx, cy);
            sched_unlock();
            last_cursor_x = cx; last_cursor_y = cy;
        }

        __asm__ volatile("pause");
    }

    /* ── Return to text shell ── */
    vga_print_hook = (void*)0;
    vga_color_hook = (void*)0;

    /* Stop mouse streaming and drain the 8042 output buffer.
       Without this, unread mouse bytes keep OBF=1, blocking the 8042 from
       accepting keyboard scancodes — IRQ1 never fires, text shell can't type. */
    mouse_disable();

    /* Drain any keyboard chars that were buffered during WM — they were
       meant for WM input and must not replay into the text shell. */
    while (keyboard_getchar()) {}

    /* Kill any background ELF tasks still running — they would otherwise
       steal keyboard input from the text shell after WM exit. */
    {
        task_t *tasks = sched_get_tasks();
        sched_lock();
        for (int i = 1; i < MAX_TASKS; i++) {
            if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING)
                tasks[i].state = TASK_DEAD;
        }
        elf_running       = 0;
        wm_elf_out_win    = -1;
        wm_elf_gfx_active = 0;
        sched_unlock();
    }

    /* Mark all WM windows dead so re-entry starts fresh */
    for (int i = 0; i < WM_MAX_WIN; i++) wm_wins[i].alive = 0;
    wm_focused  = -1;
    wm_shell_id = -1;
    /* Restore the pixel-terminal view */
    vga_redraw();
}
