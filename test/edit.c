/* IronKernel — test/edit.c
   Pixel-mode text editor using ikgfx.
   Usage: exec EDIT.ELF FILENAME.EXT

   Controls:
     Arrow keys        move cursor
     Printable chars   insert at cursor
     Backspace         delete char before cursor
     Enter             insert newline / split line
     Ctrl+S  (0x13)   save to disk
     Ctrl+X  (0x18)   exit                              */

#include <stdint.h>
#include <ikgfx.h>
#include <ironkernel.h>

/* ── Layout ─────────────────────────────────────────────────────── */
#define WIN_CW   552          /* client width  (matches wm_spawn window) */
#define WIN_CH   527          /* client height */

#define CHAR_W     8          /* font glyph width  (IBM 8×8 VGA font)   */
#define CHAR_H     9          /* font glyph height + 1 px gap            */

#define STAT_H    16          /* status bar height at bottom              */
#define TEXT_H   (WIN_CH - STAT_H)

/* Gutter: 3-digit line number, right-aligned */
#define GUT_DIGITS  3
#define GUT_W      (GUT_DIGITS * CHAR_W + 6)   /* 30 px */
#define TEXT_X     GUT_W
#define TEXT_PX    (WIN_CW - TEXT_X - 2)       /* usable text pixels */
#define COLS_VIS   (TEXT_PX / CHAR_W)          /* ~65 cols */
#define ROWS_VIS   (TEXT_H  / CHAR_H)          /* ~56 rows */

/* ── Buffer limits ───────────────────────────────────────────────── */
#define MAX_LINES  256
#define LINE_MAX   128

/* ── Colors ──────────────────────────────────────────────────────── */
#define C_BG        0x060B14u
#define C_BG2       0x030608u
#define C_TEXT      0xC8D4E8u
#define C_GUT_BG    0x080D1Au
#define C_GUT_TXT   0x374E74u
#define C_CUR_LINE  0x0C1628u
#define C_CURSOR    0x4070C8u
#define C_STAT_BG   0x0A1626u
#define C_STAT_TXT  0x70A8F0u
#define C_MOD       0xFF8844u
#define C_SEP       0x18284Au

/* ── Key codes ───────────────────────────────────────────────────── */
#define K_UP     IK_KEY_UP     /* 0x01 */
#define K_DOWN   IK_KEY_DOWN   /* 0x02 */
#define K_LEFT   IK_KEY_LEFT   /* 0x03 */
#define K_RIGHT  IK_KEY_RIGHT  /* 0x04 */
#define K_BACK   IK_KEY_BACK   /* 0x08 */
#define K_ENTER  IK_KEY_ENTER  /* 0x0A */
#define K_CTRL_S 0x13
#define K_CTRL_X 0x18

/* ── Editor state ────────────────────────────────────────────────── */
static char lines[MAX_LINES][LINE_MAX];
static int  llen[MAX_LINES];
static int  nlines   = 1;
static int  cur_r    = 0;   /* cursor row    */
static int  cur_c    = 0;   /* cursor column */
static int  top_r    = 0;   /* first visible row */
static int  modified = 0;
static char fname[32];

/* Scratch buffer for file I/O */
static uint8_t filebuf[MAX_LINES * LINE_MAX];

/* ── Minimal helpers ─────────────────────────────────────────────── */
static int ed_slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void ed_itoa3(int n, char *out)
{
    /* Fixed-width 3-digit string, space-padded on the left */
    if (n < 0) n = 0;
    out[0] = (n >= 100) ? (char)('0' + n / 100) : ' ';
    out[1] = (n >= 10)  ? (char)('0' + (n / 10) % 10) : ' ';
    out[2] = (char)('0' + n % 10);
    out[3] = 0;
}

static void ed_itoa(int n, char *out)
{
    if (n == 0) { out[0] = '0'; out[1] = 0; return; }
    char tmp[8]; int i = 0;
    while (n > 0) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    int j = 0; while (i > 0) out[j++] = tmp[--i]; out[j] = 0;
}

/* ── File I/O ────────────────────────────────────────────────────── */
static void load_file(void)
{
    nlines   = 1;
    llen[0]  = 0;
    lines[0][0] = 0;

    uint64_t sz = ik_read_file(fname, filebuf, sizeof(filebuf) - 1);
    if (sz == (uint64_t)-1 || sz == 0) return;  /* new / empty file */
    filebuf[sz] = 0;

    int r = 0, c = 0;
    for (uint64_t i = 0; i < sz && r < MAX_LINES - 1; i++) {
        uint8_t ch = filebuf[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            lines[r][c] = 0; llen[r] = c;
            r++; c = 0;
            lines[r][0] = 0; llen[r] = 0;
        } else if (c < LINE_MAX - 1) {
            lines[r][c++] = (char)ch;
        }
    }
    lines[r][c] = 0;
    llen[r]  = c;
    nlines   = r + 1;
}

static void save_file(void)
{
    int pos = 0;
    int cap = (int)sizeof(filebuf) - 1;
    for (int i = 0; i < nlines; i++) {
        for (int j = 0; j < llen[i] && pos < cap; j++)
            filebuf[pos++] = (uint8_t)lines[i][j];
        if (i < nlines - 1 && pos < cap)
            filebuf[pos++] = '\n';
    }
    ik_write_file(fname, filebuf, (uint64_t)pos);
    modified = 0;
}

/* ── Scroll helpers ─────────────────────────────────────────────── */
static void scroll_to_cursor(void)
{
    if (cur_r < top_r)
        top_r = cur_r;
    if (cur_r >= top_r + ROWS_VIS)
        top_r = cur_r - ROWS_VIS + 1;
    if (top_r < 0) top_r = 0;
}

static void clamp_col(void)
{
    if (cur_c > llen[cur_r]) cur_c = llen[cur_r];
    if (cur_c < 0)           cur_c = 0;
}

/* ── Drawing ─────────────────────────────────────────────────────── */
static void draw_frame(void)
{
    /* Background */
    ik_gfx_grad(0, 0, WIN_CW, TEXT_H, C_BG, C_BG2);

    /* Gutter */
    ik_gfx_rect(0, 0, GUT_W, TEXT_H, C_GUT_BG);
    ik_gfx_rect(GUT_W - 1, 0, 1, TEXT_H, C_SEP);

    /* Lines */
    for (int row = 0; row < ROWS_VIS; row++) {
        int li = top_r + row;
        if (li >= nlines) break;
        int py = row * CHAR_H;

        /* Highlight current line background */
        if (li == cur_r)
            ik_gfx_rect(GUT_W, py, WIN_CW - GUT_W, CHAR_H, C_CUR_LINE);

        /* Line number — right-aligned in gutter */
        char nbuf[4];
        ed_itoa3(li + 1, nbuf);
        ik_gfx_str(2, py, nbuf, C_GUT_TXT);

        /* Text content (truncated at COLS_VIS) */
        if (llen[li] > 0) {
            int vislen = llen[li];
            if (vislen > COLS_VIS) vislen = COLS_VIS;
            char tmp[LINE_MAX];
            int k;
            for (k = 0; k < vislen; k++) tmp[k] = lines[li][k];
            tmp[k] = 0;
            ik_gfx_str(TEXT_X, py, tmp, C_TEXT);
        }

        /* Cursor: vertical bar on current line */
        if (li == cur_r && cur_c <= COLS_VIS) {
            int cx = TEXT_X + cur_c * CHAR_W;
            ik_gfx_rect(cx, py, 2, CHAR_H, C_CURSOR);
        }
    }

    /* Status bar */
    int sy = TEXT_H;
    ik_gfx_rect(0, sy, WIN_CW, STAT_H, C_STAT_BG);
    ik_gfx_rect(0, sy, WIN_CW, 1, C_SEP);

    /* Build status string: "FILENAME [+]  Ln:N Col:N" */
    char stat[72];
    int  si = 0;

    /* Filename */
    const char *fp = fname;
    while (*fp && si < 20) stat[si++] = *fp++;
    if (modified) {
        stat[si++] = ' '; stat[si++] = '['; stat[si++] = '+'; stat[si++] = ']';
    }
    stat[si++] = ' '; stat[si++] = ' ';

    /* Ln:N */
    stat[si++] = 'L'; stat[si++] = 'n'; stat[si++] = ':';
    char nb[8]; ed_itoa(cur_r + 1, nb);
    int  k = 0; while (nb[k]) stat[si++] = nb[k++];
    stat[si++] = ' ';

    /* Col:N */
    stat[si++] = 'C'; stat[si++] = 'o'; stat[si++] = 'l'; stat[si++] = ':';
    ed_itoa(cur_c + 1, nb);
    k = 0; while (nb[k]) stat[si++] = nb[k++];
    stat[si] = 0;

    ik_gfx_str(4, sy + 4, stat, modified ? C_MOD : C_STAT_TXT);

    /* Right-side hint */
    const char *hint = "^S Save  ^X Exit";
    int hx = WIN_CW - ed_slen(hint) * CHAR_W - 4;
    ik_gfx_str(hx, sy + 4, hint, C_GUT_TXT);

    ik_gfx_flush();
}

/* ── Key handling ────────────────────────────────────────────────── */
static void handle_key(int k)
{
    switch (k) {

    case K_UP:
        if (cur_r > 0) { cur_r--; clamp_col(); scroll_to_cursor(); }
        break;

    case K_DOWN:
        if (cur_r < nlines - 1) { cur_r++; clamp_col(); scroll_to_cursor(); }
        break;

    case K_LEFT:
        if (cur_c > 0) {
            cur_c--;
        } else if (cur_r > 0) {
            cur_r--;
            cur_c = llen[cur_r];
            scroll_to_cursor();
        }
        break;

    case K_RIGHT:
        if (cur_c < llen[cur_r]) {
            cur_c++;
        } else if (cur_r < nlines - 1) {
            cur_r++;
            cur_c = 0;
            scroll_to_cursor();
        }
        break;

    case K_BACK:
        if (cur_c > 0) {
            /* Delete char before cursor */
            char *ln = lines[cur_r];
            int   l  = llen[cur_r];
            for (int i = cur_c - 1; i < l - 1; i++) ln[i] = ln[i + 1];
            ln[l - 1] = 0;
            llen[cur_r]--;
            cur_c--;
            modified = 1;
        } else if (cur_r > 0) {
            /* Merge current line into previous */
            int prev = cur_r - 1;
            int plen = llen[prev];
            int clen = llen[cur_r];
            if (plen + clen < LINE_MAX - 1) {
                for (int i = 0; i < clen; i++)
                    lines[prev][plen + i] = lines[cur_r][i];
                llen[prev] = plen + clen;
                lines[prev][plen + clen] = 0;
                /* Shift subsequent lines up */
                for (int i = cur_r; i < nlines - 1; i++) {
                    for (int j = 0; j <= llen[i + 1]; j++)
                        lines[i][j] = lines[i + 1][j];
                    llen[i] = llen[i + 1];
                }
                nlines--;
                cur_r--;
                cur_c = plen;
                scroll_to_cursor();
                modified = 1;
            }
        }
        break;

    case K_ENTER:
        if (nlines < MAX_LINES - 1) {
            /* Split line at cursor: rest goes to new line below */
            int rest = llen[cur_r] - cur_c;
            /* Shift lines down */
            for (int i = nlines - 1; i > cur_r; i--) {
                for (int j = 0; j <= llen[i]; j++)
                    lines[i + 1][j] = lines[i][j];
                llen[i + 1] = llen[i];
            }
            /* New line = rest of current */
            for (int i = 0; i < rest; i++)
                lines[cur_r + 1][i] = lines[cur_r][cur_c + i];
            lines[cur_r + 1][rest] = 0;
            llen[cur_r + 1] = rest;
            /* Truncate current line at cursor */
            lines[cur_r][cur_c] = 0;
            llen[cur_r] = cur_c;
            nlines++;
            cur_r++;
            cur_c = 0;
            scroll_to_cursor();
            modified = 1;
        }
        break;

    default:
        /* Insert printable character */
        if (k >= 0x20 && k < 0x7F) {
            char *ln = lines[cur_r];
            int   l  = llen[cur_r];
            if (l < LINE_MAX - 1) {
                /* Shift right from cursor */
                for (int i = l; i > cur_c; i--) ln[i] = ln[i - 1];
                ln[cur_c] = (char)k;
                ln[l + 1] = 0;
                llen[cur_r]++;
                cur_c++;
                modified = 1;
            }
        }
        break;
    }
}

/* ── Entry point ─────────────────────────────────────────────────── */
int main(void)
{
    /* Get filename from exec argument */
    char arg[32];
    ik_get_arg(arg, sizeof(arg));

    if (arg[0]) {
        int i = 0;
        while (arg[i] && i < 31) { fname[i] = arg[i]; i++; }
        fname[i] = 0;
    } else {
        /* No argument — use a default filename */
        const char *def = "UNTITLED.TXT";
        int i = 0;
        while (def[i]) { fname[i] = def[i]; i++; }
        fname[i] = 0;
    }

    ik_gfx_init();
    load_file();
    draw_frame();

    for (;;) {
        int k = ik_read_key();

        if (k == K_CTRL_X) break;

        if (k == K_CTRL_S) {
            save_file();
            draw_frame();
            continue;
        }

        handle_key(k);
        draw_frame();
    }

    return 0;
}
