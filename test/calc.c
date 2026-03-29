/* IronKernel GUI Calculator — test/calc.c
   Pixel-mode calculator using ikgfx.h.
   Window: 280×370, client: 278×351.
   Layout: display + 4×4 button grid. */

#include <stdint.h>
#include <stdlib.h>   /* itoa */
#include <ikgfx.h>

/* ── Layout constants ─────────────────────────────────────── */
#define WIN_CW   278   /* client width  */
#define WIN_CH   351   /* client height */

#define MAR      6     /* left/right margin */
#define DY       8     /* display top Y     */
#define DW       (WIN_CW - 2 * MAR)   /* display width  = 266 */
#define DH       38    /* display height     */

#define BY       (DY + DH + 8)        /* buttons start Y = 54 */
#define COLS     4
#define ROWS     4
#define GAP      4

/* Button dimensions: fit 4 cols + 3 gaps in DW */
#define BW       ((DW - (COLS-1)*GAP) / COLS)   /* ~62 */
#define BH       ((WIN_CH - BY - (ROWS-1)*GAP - 6) / ROWS)  /* ~69 */

#define BTN_X(c) (MAR + (c) * (BW + GAP))
#define BTN_Y(r) (BY  + (r) * (BH + GAP))

/* ── Button grid ──────────────────────────────────────────── */
static const char *const btn_label[ROWS][COLS] = {
    { "7",   "8", "9", "/"  },
    { "4",   "5", "6", "*"  },
    { "1",   "2", "3", "-"  },
    { "0",   "CLR","=", "+" },
};

/* ── Calculator state ─────────────────────────────────────── */
static long long accum = 0;
static long long entry = 0;
static char      op    = 0;
static int       fresh = 1;   /* 1 = next digit starts new entry */
static char      disp_buf[24];

/* Format a long long as decimal without truncation */
static void lltoa_dec(long long n, char *buf)
{
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    char tmp[22]; int i = 21; tmp[21] = '\0';
    int neg = (n < 0);
    unsigned long long u = neg ? (unsigned long long)(-n) : (unsigned long long)n;
    while (u > 0) { tmp[--i] = (char)('0' + (int)(u % 10)); u /= 10; }
    if (neg) tmp[--i] = '-';
    int j = 0;
    while (tmp[i]) buf[j++] = tmp[i++];
    buf[j] = '\0';
}

static void update_disp(void)
{
    lltoa_dec(entry, disp_buf);
}

/* ── Draw helpers ─────────────────────────────────────────── */
static int slen(const char *s) { int n=0; while(s[n]) n++; return n; }

static void draw_display(void)
{
    ik_textbox(MAR, DY, DW, DH);
    /* Right-align number text */
    int tw  = slen(disp_buf) * 8;
    int tx  = MAR + DW - tw - 6;
    int ty  = DY + (DH - 8) / 2;
    ik_gfx_str(tx, ty, disp_buf, IK_ACCENT);
}

static void draw_button(int c, int r)
{
    int bx = BTN_X(c), by = BTN_Y(r);
    const char *lbl = btn_label[r][c];
    char ch = lbl[0];

    if (ch == '=') {
        ik_button_ex(bx, by, BW, BH, lbl, IK_EQHI, IK_EQLO);
    } else if (ch == '/' || ch == '*' || ch == '-' || ch == '+') {
        ik_button_ex(bx, by, BW, BH, lbl, IK_OPHI, IK_OPLO);
    } else if (ch == 'C') {   /* CLR */
        ik_button_ex(bx, by, BW, BH, lbl, 0xC04040u, 0x801010u);
    } else {
        ik_button(bx, by, BW, BH, lbl);
    }
}

/* Actual client dimensions as provided by the WM (554x546 window → 552x527 client) */
#define IK_CLIENT_W  552
#define IK_CLIENT_H  527

static void draw_all(void)
{
    /* Background gradient — fill the full client area so the right/bottom
       portions are not left as the uninitialized dark-blue init color. */
    ik_gfx_grad(0, 0, IK_CLIENT_W, IK_CLIENT_H, 0x0A1422u, 0x060C18u);

    /* Title label */
    ik_label(MAR, DY - 0, "Calculator", IK_ACCENT);

    draw_display();

    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            draw_button(c, r);

    ik_gfx_flush();
}

/* ── Click handler ────────────────────────────────────────── */
static void handle_click(uint32_t click)
{
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            if (!ik_hit(click, BTN_X(c), BTN_Y(r), BW, BH))
                continue;

            const char *lbl = btn_label[r][c];
            char ch = lbl[0];

            if (ch >= '0' && ch <= '9') {
                if (fresh) { entry = 0; fresh = 0; }
                entry = entry * 10 + (ch - '0');

            } else if (ch == 'C') {
                accum = 0; entry = 0; op = 0; fresh = 1;

            } else if (ch == '=') {
                if (op) {
                    if      (op == '+') accum += entry;
                    else if (op == '-') accum -= entry;
                    else if (op == '*') accum *= entry;
                    else if (op == '/' && entry) accum /= entry;
                    entry = accum;
                    op    = 0;
                }
                fresh = 1;

            } else {  /* operator: + - * / */
                if (!fresh) {
                    /* chain: apply pending op before storing new one */
                    if (!op) accum = entry;
                    else {
                        if      (op == '+') accum += entry;
                        else if (op == '-') accum -= entry;
                        else if (op == '*') accum *= entry;
                        else if (op == '/' && entry) accum /= entry;
                    }
                } else if (!op) {
                    accum = entry;
                }
                op    = ch;
                fresh = 1;
            }

            update_disp();
            return;
        }
    }
}

/* ── Entry point ──────────────────────────────────────────── */
int main(void)
{
    ik_gfx_init();

    accum = 0; entry = 0; op = 0; fresh = 1;
    disp_buf[0] = '0'; disp_buf[1] = '\0';

    draw_all();

    while (1) {
        uint32_t click = ik_get_click();
        handle_click(click);
        draw_all();
    }

    return 0;
}
