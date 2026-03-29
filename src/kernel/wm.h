#ifndef WM_H
#define WM_H

#include "../kernel/types.h"

/* Maximum number of simultaneous windows */
#define WM_MAX_WIN   12

/* Screen dimensions — duplicated here for syscall.c access */
#define SCR_W  800
#define SCR_H  600

/* Text-buffer dimensions per window */
#define WM_BUF_ROWS  80
#define WM_BUF_COLS  100

typedef struct {
    int alive;
    int x, y, w, h;        /* screen position / size              */
    char title[32];

    /* Text content */
    char    buf[WM_BUF_ROWS][WM_BUF_COLS];
    uint32_t col[WM_BUF_ROWS][WM_BUF_COLS]; /* fg colour (32bpp RGB) per cell */
    int     cur_col, cur_row;               /* write cursor (absolute)  */
    int     vis_cols, vis_rows;             /* visible chars            */
    int     top_row;                        /* first visible row        */

    /* Drag state */
    int dragging;
    int drag_ox, drag_oy;

    /* Window state flags */
    int minimized;  /* 1 = hidden to taskbar, 0 = visible */

    /* Set when text is written by an ELF task; cleared after WM redraw */
    int dirty;
} wm_win_t;

extern int      wm_focused;
extern int      wm_z_top;         /* index of visually topmost window  */
extern wm_win_t wm_wins[WM_MAX_WIN];
extern uint32_t wm_cur_fg;        /* current text fg colour (32bpp RGB) for hook output */
extern int      wm_is_running;    /* 1 once wm_run() has been entered */

/* Create a window; returns index or -1 */
int  wm_create(int x, int y, int w, int h, const char *title);

/* Write a character / string to a window's text buffer */
void wm_putchar(int id, char c);
void wm_puts(int id, const char *s);
void wm_puts_col(int id, const char *s, uint32_t color);
void wm_backspace(int id);

/* Spawn an ELF in its own window from the GUI shell exec command */
void wm_spawn_elf(const char *cmd);

/* Main entry point — shows splash then runs desktop; never returns */
void wm_run(void);

/* ── ELF pixel-buffer GFX ──────────────────────────────────────────
   One global pixel buffer serves the currently-running ELF GFX window.
   Activated by SYS_WIN_GFX_INIT; cleared when ELF exits.
   Dimensions are capped to ELF_GFX_MAXW × ELF_GFX_MAXH.           */
#define WM_ELF_GFX_MAXW  552
#define WM_ELF_GFX_MAXH  527

extern uint32_t       wm_elf_gfx_buf[WM_ELF_GFX_MAXH * WM_ELF_GFX_MAXW];
extern int            wm_elf_gfx_cw;   /* actual client width  (px) */
extern int            wm_elf_gfx_ch;   /* actual client height (px) */
extern volatile int   wm_elf_gfx_active; /* 1 while ELF GFX mode is on */

#endif
