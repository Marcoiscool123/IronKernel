/* IronKernel GUI Shell — test/shell2.c
   Pixel-mode terminal emulator using ikgfx.h.
   Window: 554×546, client: 552×527.

   Features: ls, cd, pwd, cat, mkdir, rm, touch, echo,
             uptime, mem, clear, help, exit.
   Scrollback: UP/DOWN arrows.  No external dependencies beyond iklibc. */

#include <stdint.h>
#include <ikgfx.h>
#include <ironkernel.h>

/* ── Window / font geometry ────────────────────────────────────── */
#define WIN_CW   552
#define WIN_CH   527
#define PAD_X      4
#define PAD_Y      4
#define CHW        8    /* glyph width  */
#define CHH        9    /* glyph height (8px glyph + 1px leading) */
#define TCOLS     68    /* 4 + 68*8 + 4 = 552  ✓ */
#define TVIS      56    /* visible output rows   */
/* separator row Y and input text Y */
#define TSEP_Y   (PAD_Y + TVIS * CHH)    /* 4 + 504 = 508 */
#define TINP_Y   (TSEP_Y + 2)            /* 510           */

/* ── Scrollback buffer ─────────────────────────────────────────── */
#define TBUF_ROWS 350
static char     tb_text [TBUF_ROWS][TCOLS + 1];
static uint32_t tb_color[TBUF_ROWS];
static int      tb_head  = 0;  /* next slot to write  */
static int      tb_rows  = 0;  /* rows stored so far  */
static int      tb_scroll = 0; /* rows scrolled up    */

/* ── Input line ────────────────────────────────────────────────── */
#define INP_MAX 126
static char inp[INP_MAX + 2];
static int  inp_len = 0;

/* ── Working directory cache ───────────────────────────────────── */
static char cwd[64];

/* ── File read scratch buffer ──────────────────────────────────── */
static char cat_buf[8192];

/* ── Directory listing scratch ─────────────────────────────────── */
static ik_dirent_t dir_buf[80];

/* ── Colors ────────────────────────────────────────────────────── */
#define COL_TEXT   0xC8D4E8u
#define COL_DIM    0x607888u
#define COL_HEAD   0x70A8F0u
#define COL_PROMPT 0x22CCFFu
#define COL_PATH   0x66EE88u
#define COL_ERROR  0xFF6060u
#define COL_OK     0x44DD88u
#define COL_WARN   0xFFCC44u
#define COL_DIR    0x55BBFFu
#define COL_FILE   0xB0CCE8u
#define COL_ELF    0xAAEEAAu

/* ═══════════════════════════ Utilities ════════════════════════════ */

static int gsh_strlen(const char *s)
{ int n=0; while(s[n]) n++; return n; }

static void gsh_strcpy(char *d, const char *s)
{ while ((*d++ = *s++)); }

static void gsh_strcat(char *d, const char *s)
{ while (*d) d++; while ((*d++ = *s++)); }

static int gsh_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static int gsh_strncmp(const char *a, const char *b, int n) {
    while (n-- > 0 && *a && *a == *b) { a++; b++; }
    return (n < 0) ? 0 : (unsigned char)*a - (unsigned char)*b;
}

static int gsh_isspace(char c)
{ return c==' '||c=='\t'; }

static void gsh_itoa(uint64_t n, char *buf) {
    if (!n) { buf[0]='0'; buf[1]=0; return; }
    char tmp[24]; int i=0;
    while (n) { tmp[i++]=(char)('0'+(int)(n%10)); n/=10; }
    int j=0; while(i>0) buf[j++]=tmp[--i]; buf[j]=0;
}

/* Upper-case a char */
static char gsh_upper(char c)
{ return (c>='a'&&c<='z')?(char)(c-32):c; }

/* ═══════════════════════════ Scrollback ═══════════════════════════ */

/* Append one physical row to the ring buffer */
static void tb_newrow(const char *text, uint32_t color)
{
    int i = 0;
    while (i < TCOLS && text[i]) { tb_text[tb_head][i] = text[i]; i++; }
    tb_text[tb_head][i] = 0;
    tb_color[tb_head]   = color;
    tb_head = (tb_head + 1) % TBUF_ROWS;
    if (tb_rows < TBUF_ROWS) tb_rows++;
    /* Keep view stable when user has scrolled up */
    if (tb_scroll > 0) tb_scroll++;
}

/* Print text with wrapping; '\n' forces a new row */
static void gsh_puts(const char *s, uint32_t color)
{
    char line[TCOLS + 1];
    int  li = 0;
    while (*s) {
        if (*s == '\n' || li >= TCOLS) {
            line[li] = 0;
            tb_newrow(line, color);
            li = 0;
            if (*s == '\n') { s++; continue; }
        } else {
            line[li++] = *s++;
        }
    }
    if (li > 0) { line[li] = 0; tb_newrow(line, color); }
}

/* Append a right-padded label + value pair on one row */
static void gsh_kv(const char *key, const char *val, uint32_t vc)
{
    char buf[TCOLS+1];
    buf[0]=' '; buf[1]=' '; buf[2]=0;
    gsh_strcat(buf, key);
    /* pad to column 22 */
    int l = gsh_strlen(buf);
    while (l < 22) { buf[l++]=' '; } buf[l]=0;
    gsh_strcat(buf, val);
    tb_newrow(buf, vc);
}

/* ═══════════════════════════ Rendering ════════════════════════════ */

/* Full render: background + all output rows + input line.
   Use after commands complete or on first draw. */
static void gsh_render(void)
{
    /* ── Background gradient ── */
    ik_gfx_grad(0, 0, WIN_CW, WIN_CH, 0x05080Eu, 0x020407u);

    /* ── Output area ── */
    {
        /* logical index of bottom-most visible row */
        int bot = tb_rows - 1 - tb_scroll;
        int top = bot - TVIS + 1;
        for (int r = 0; r < TVIS; r++) {
            int li = top + r;
            if (li < 0 || li >= tb_rows) continue;
            int bi = (tb_head - tb_rows + li + TBUF_ROWS * 2) % TBUF_ROWS;
            ik_gfx_str(PAD_X, PAD_Y + r * CHH, tb_text[bi], tb_color[bi]);
        }
    }

    /* ── Separator ── */
    ik_gfx_rect(PAD_X, TSEP_Y,   WIN_CW - PAD_X*2, 1, 0x1A2848u);
    ik_gfx_rect(PAD_X, TSEP_Y+1, WIN_CW - PAD_X*2, 1, 0x304868u);

    /* ── Scroll indicator ── */
    if (tb_scroll > 0) {
        char scind[12];
        scind[0]='^'; scind[1]='^'; scind[2]=' ';
        gsh_itoa((uint64_t)tb_scroll, scind+3);
        ik_gfx_str(WIN_CW - 64, TSEP_Y - CHH, scind, COL_WARN);
    }

    /* ── Prompt + input ── */
    {
        int px = PAD_X;
        int py = TINP_Y;

        /* "IK@" */
        ik_gfx_str(px, py, "IK@", COL_PROMPT);
        px += 3 * CHW;

        /* cwd (cap at 20 chars) */
        const char *pcwd = cwd[0] ? cwd : "/";
        int cwdlen = gsh_strlen(pcwd);
        if (cwdlen > 20) { pcwd += cwdlen - 20; cwdlen = 20; }
        ik_gfx_str(px, py, pcwd, COL_PATH);
        px += cwdlen * CHW;

        /* "> " */
        ik_gfx_str(px, py, "> ", COL_TEXT);
        px += 2 * CHW;

        /* input text */
        if (inp_len > 0) {
            inp[inp_len] = 0;
            ik_gfx_str(px, py, inp, COL_TEXT);
            px += inp_len * CHW;
        }

        /* Cursor bar */
        ik_gfx_rect(px, py, 2, CHH - 1, COL_PROMPT);
    }

    ik_gfx_flush();
}

/* Fast input-only render: only redraws separator + prompt + input line.
   Does NOT redraw background or output rows — they persist in the buffer.
   Call this while the user is typing to avoid the expensive full redraw. */
static void gsh_render_input(void)
{
    /* Erase input row with a background-coloured rect */
    ik_gfx_rect(0, TSEP_Y - 1, WIN_CW, WIN_CH - TSEP_Y + 1, 0x05080Eu);

    /* Redraw separator */
    ik_gfx_rect(PAD_X, TSEP_Y,   WIN_CW - PAD_X*2, 1, 0x1A2848u);
    ik_gfx_rect(PAD_X, TSEP_Y+1, WIN_CW - PAD_X*2, 1, 0x304868u);

    /* Scroll indicator */
    if (tb_scroll > 0) {
        char scind[12];
        scind[0]='^'; scind[1]='^'; scind[2]=' ';
        gsh_itoa((uint64_t)tb_scroll, scind+3);
        ik_gfx_str(WIN_CW - 64, TSEP_Y - CHH, scind, COL_WARN);
    }

    /* Prompt + input line */
    {
        int px = PAD_X, py = TINP_Y;
        ik_gfx_str(px, py, "IK@", COL_PROMPT); px += 3 * CHW;
        const char *pcwd = cwd[0] ? cwd : "/";
        int cwdlen = gsh_strlen(pcwd);
        if (cwdlen > 20) { pcwd += cwdlen - 20; cwdlen = 20; }
        ik_gfx_str(px, py, pcwd, COL_PATH); px += cwdlen * CHW;
        ik_gfx_str(px, py, "> ", COL_TEXT); px += 2 * CHW;
        if (inp_len > 0) {
            inp[inp_len] = 0;
            ik_gfx_str(px, py, inp, COL_TEXT);
            px += inp_len * CHW;
        }
        ik_gfx_rect(px, py, 2, CHH - 1, COL_PROMPT);
    }

    ik_gfx_flush();
}

/* ═══════════════════════════ Input ════════════════════════════════ */

static void gsh_getline(void)
{
    inp_len = 0;
    gsh_render();   /* full render once at prompt */
    for (;;) {
        int k = ik_read_key();
        if (k == IK_KEY_ENTER) {
            tb_scroll = 0;
            break;
        }
        int need_full = 0;
        if (k == IK_KEY_BACK) {
            if (inp_len > 0) inp_len--;
        } else if (k == IK_KEY_UP) {
            if (tb_scroll + TVIS < tb_rows) { tb_scroll++; need_full = 1; }
        } else if (k == IK_KEY_DOWN) {
            if (tb_scroll > 0) { tb_scroll--; need_full = 1; }
        } else if ((unsigned char)k >= 0x20 && (unsigned char)k < 0x7F
                   && inp_len < INP_MAX) {
            inp[inp_len++] = (char)k;
        }
        /* Scroll changes the output area → full redraw; typing → input only */
        if (need_full) gsh_render();
        else           gsh_render_input();
    }
    inp[inp_len] = 0;
}

/* ═══════════════════════════ Arg parsing ══════════════════════════ */

/* Copy first whitespace-delimited token after the command word */
static int gsh_arg(const char *line, int skip, char *out, int max)
{
    /* skip <skip> words */
    for (int w = 0; w <= skip; w++) {
        while (*line && !gsh_isspace(*line)) line++;
        while (*line && gsh_isspace(*line))  line++;
    }
    /* go back one: skip already advanced past the command, we want arg 0 */
    /* Actually: skip=0 → copy word after command */
    int i = 0;
    while (*line && !gsh_isspace(*line) && i < max-1) out[i++] = *line++;
    out[i] = 0;
    return i;
}

/* Copy first argument (word after the command) */
static int gsh_arg1(const char *line, char *out, int max)
{
    /* Skip command word */
    while (*line && !gsh_isspace(*line)) line++;
    while (*line && gsh_isspace(*line))  line++;
    int i = 0;
    while (*line && !gsh_isspace(*line) && i < max-1) out[i++] = *line++;
    out[i] = 0;
    return i;
}

/* Copy rest of line after command word (for echo) */
static void gsh_rest(const char *line, char *out, int max)
{
    while (*line && !gsh_isspace(*line)) line++;
    while (*line && gsh_isspace(*line))  line++;
    int i = 0;
    while (*line && i < max-1) out[i++] = *line++;
    out[i] = 0;
}

/* ═══════════════════════════ Commands ═════════════════════════════ */

static void cmd_help(const char *line)
{
    (void)line;
    gsh_puts("", 0);
    gsh_puts("  IronKernel GUI Shell", COL_HEAD);
    gsh_puts("  ───────────────────────────────────", COL_DIM);
    gsh_kv("  ls [dir]",   "list directory",      COL_TEXT);
    gsh_kv("  cd <dir>",   "change directory",    COL_TEXT);
    gsh_kv("  pwd",        "print working dir",   COL_TEXT);
    gsh_kv("  cat <file>", "print file contents", COL_TEXT);
    gsh_kv("  mkdir <d>",  "create directory",    COL_TEXT);
    gsh_kv("  rm <file>",  "delete file",         COL_TEXT);
    gsh_kv("  touch <f>",  "create empty file",   COL_TEXT);
    gsh_kv("  echo <txt>", "print text",          COL_TEXT);
    gsh_kv("  uptime",     "system uptime",       COL_TEXT);
    gsh_kv("  mem",        "memory statistics",   COL_TEXT);
    gsh_kv("  clear",      "clear terminal",      COL_TEXT);
    gsh_kv("  exit",       "close this shell",    COL_TEXT);
    gsh_puts("  ↑/↓ arrows scroll output.", COL_DIM);
    gsh_puts("", 0);
}

static void cmd_ls(const char *line)
{
    char arg[64];
    gsh_arg1(line, arg, 64);

    if (arg[0] && ik_chdir(arg) != 0) {
        char buf[80]; buf[0]=0;
        gsh_strcat(buf, "  ls: '"); gsh_strcat(buf, arg);
        gsh_strcat(buf, "': not found");
        gsh_puts(buf, COL_ERROR);
        return;
    }

    int n = ik_readdir(dir_buf, 80);
    gsh_puts("", 0);

    /* Two-column layout: each entry occupies 34 chars */
    char left[36]; int have_left = 0;
    for (int i = 0; i < n; i++) {
        /* Build entry string */
        char ent[36];
        int  ei = 0;
        uint32_t ec;
        if (dir_buf[i].is_dir) {
            ent[ei++]='[';
            for (int k=0; dir_buf[i].name[k]&&k<8; k++) ent[ei++]=dir_buf[i].name[k];
            /* trim trailing spaces */
            while (ei>1 && ent[ei-1]==' ') ei--;
            ent[ei++]=']';
            ec = COL_DIR;
        } else {
            for (int k=0; dir_buf[i].name[k]&&k<8; k++) ent[ei++]=dir_buf[i].name[k];
            while (ei>0 && ent[ei-1]==' ') ei--;
            if (dir_buf[i].ext[0]) {
                ent[ei++]='.';
                for (int k=0; dir_buf[i].ext[k]&&k<3; k++) ent[ei++]=dir_buf[i].ext[k];
            }
            /* colour ELFs differently */
            ec = (dir_buf[i].ext[0]=='E'&&dir_buf[i].ext[1]=='L'&&
                  dir_buf[i].ext[2]=='F') ? COL_ELF : COL_FILE;
        }
        ent[ei] = 0;
        /* Pad to 33 chars */
        while (ei < 33) { ent[ei++]=' '; } ent[ei]=0;

        if (!have_left) {
            /* store left column; colour chosen from left entry */
            gsh_strcpy(left, ent);
            have_left = 1;
        } else {
            /* combine left + right into one row */
            char row[70]; row[0]=' '; row[1]=' ';
            gsh_strcpy(row+2, left);
            gsh_strcat(row, ent);
            /* Use left colour for whole row (dirs stand out via [] ) */
            tb_newrow(row, ec);
            have_left = 0;
        }
    }
    if (have_left) {
        char row[70]; row[0]=' '; row[1]=' '; row[2]=0;
        gsh_strcat(row, left);
        tb_newrow(row, COL_FILE);
    }
    if (n == 0) gsh_puts("  (empty)", COL_DIM);
    gsh_puts("", 0);

    if (arg[0]) ik_chdir("..");
    ik_getcwd(cwd, 64);
}

static void cmd_cd(const char *line)
{
    char arg[64]; gsh_arg1(line, arg, 64);
    if (!arg[0]) { ik_chdir("/"); ik_getcwd(cwd, 64); return; }
    if (ik_chdir(arg) != 0) {
        char buf[80]; buf[0]=0;
        gsh_strcat(buf, "  cd: '"); gsh_strcat(buf, arg);
        gsh_strcat(buf, "': not found");
        gsh_puts(buf, COL_ERROR);
        return;
    }
    ik_getcwd(cwd, 64);
}

static void cmd_pwd(const char *line)
{
    (void)line;
    char buf[66]; buf[0]=' '; buf[1]=' '; buf[2]=0;
    gsh_strcat(buf, cwd[0] ? cwd : "/");
    gsh_puts(buf, COL_PATH);
}

static void cmd_cat(const char *line)
{
    char arg[64]; gsh_arg1(line, arg, 64);
    if (!arg[0]) { gsh_puts("  Usage: cat <file>", COL_ERROR); return; }
    uint64_t n = ik_read_file(arg, cat_buf, sizeof(cat_buf)-1);
    if (n == (uint64_t)-1) {
        char buf[80]; buf[0]=0;
        gsh_strcat(buf, "  cat: '"); gsh_strcat(buf, arg);
        gsh_strcat(buf, "': not found");
        gsh_puts(buf, COL_ERROR);
        return;
    }
    cat_buf[n] = 0;
    gsh_puts("", 0);
    gsh_puts(cat_buf, COL_TEXT);
    gsh_puts("", 0);
}

static void cmd_echo(const char *line)
{
    char rest[TCOLS]; gsh_rest(line, rest, TCOLS);
    char buf[TCOLS+3]; buf[0]=' '; buf[1]=' '; buf[2]=0;
    gsh_strcat(buf, rest);
    gsh_puts(buf, COL_TEXT);
}

static void cmd_mkdir(const char *line)
{
    char arg[64]; gsh_arg1(line, arg, 64);
    if (!arg[0]) { gsh_puts("  Usage: mkdir <dir>", COL_ERROR); return; }
    if (ik_mkdir(arg) != 0) {
        gsh_puts("  mkdir: failed (already exists?)", COL_ERROR);
    } else {
        char buf[72]; buf[0]=0;
        gsh_strcat(buf, "  Created directory '"); gsh_strcat(buf, arg);
        gsh_strcat(buf, "'");
        gsh_puts(buf, COL_OK);
    }
}

static void cmd_rm(const char *line)
{
    char arg[64]; gsh_arg1(line, arg, 64);
    if (!arg[0]) { gsh_puts("  Usage: rm <file>", COL_ERROR); return; }
    if (ik_delete(arg) != 0) {
        char buf[80]; buf[0]=0;
        gsh_strcat(buf, "  rm: '"); gsh_strcat(buf, arg);
        gsh_strcat(buf, "': not found");
        gsh_puts(buf, COL_ERROR);
    } else {
        char buf[72]; buf[0]=0;
        gsh_strcat(buf, "  Deleted '"); gsh_strcat(buf, arg); gsh_strcat(buf, "'");
        gsh_puts(buf, COL_OK);
    }
}

static void cmd_touch(const char *line)
{
    char arg[64]; gsh_arg1(line, arg, 64);
    if (!arg[0]) { gsh_puts("  Usage: touch <file>", COL_ERROR); return; }
    if (ik_write_file(arg, "", 0) != 0) {
        gsh_puts("  touch: write failed", COL_ERROR);
    } else {
        char buf[72]; buf[0]=0;
        gsh_strcat(buf, "  Created '"); gsh_strcat(buf, arg); gsh_strcat(buf, "'");
        gsh_puts(buf, COL_OK);
    }
}

static void cmd_uptime(const char *line)
{
    (void)line;
    uint64_t t    = ik_uptime();
    uint64_t secs = t / 100;
    uint64_t mins = secs / 60; secs %= 60;
    uint64_t hrs  = mins / 60; mins %= 60;
    char buf[64]; char tmp[24];
    buf[0]=' '; buf[1]=' '; buf[2]=0;
    gsh_itoa(hrs,  tmp); gsh_strcat(buf, tmp); gsh_strcat(buf, "h ");
    gsh_itoa(mins, tmp); gsh_strcat(buf, tmp); gsh_strcat(buf, "m ");
    gsh_itoa(secs, tmp); gsh_strcat(buf, tmp); gsh_strcat(buf, "s");
    gsh_puts(buf, COL_WARN);
}

static void cmd_mem(const char *line)
{
    (void)line;
    ik_meminfo_t m; ik_meminfo(&m);
    char buf[80]; char tmp[24];

    buf[0]=' '; buf[1]=' '; buf[2]=0;
    gsh_itoa(m.total_kb / 1024, tmp); gsh_strcat(buf, tmp); gsh_strcat(buf, " MB total");
    gsh_puts(buf, COL_TEXT);

    buf[0]=' '; buf[1]=' '; buf[2]=0;
    gsh_itoa(m.free_kb / 1024, tmp); gsh_strcat(buf, tmp);
    gsh_strcat(buf, " MB free  (");
    gsh_itoa(m.free_kb * 100 / (m.total_kb ? m.total_kb : 1), tmp);
    gsh_strcat(buf, tmp); gsh_strcat(buf, "%)");
    gsh_puts(buf, COL_WARN);
}

static void cmd_clear(const char *line)
{
    (void)line;
    tb_head = 0; tb_rows = 0; tb_scroll = 0;
}

/* ═══════════════════════════ Dispatcher ═══════════════════════════ */

#define CMD(name, n, fn)  \
    if (gsh_strncmp(line, name, n)==0 && (line[n]==0||gsh_isspace(line[n]))) \
        { fn(line); return; }

static void dispatch(const char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;

    /* Echo typed command dimly */
    {
        char buf[TCOLS+3]; buf[0]=' '; buf[1]=' '; buf[2]=0;
        int i=2; const char *p=line;
        while (*p && i < TCOLS+1) buf[i++]=*p++;
        buf[i]=0;
        tb_newrow(buf, COL_DIM);
    }

    CMD("help",   4, cmd_help)
    CMD("ls",     2, cmd_ls)
    CMD("cd",     2, cmd_cd)
    CMD("pwd",    3, cmd_pwd)
    CMD("cat",    3, cmd_cat)
    CMD("echo",   4, cmd_echo)
    CMD("mkdir",  5, cmd_mkdir)
    CMD("rm",     2, cmd_rm)
    CMD("touch",  5, cmd_touch)
    CMD("uptime", 6, cmd_uptime)
    CMD("mem",    3, cmd_mem)
    CMD("clear",  5, cmd_clear)

    /* Unknown command */
    {
        char buf[80]; buf[0]=0;
        gsh_strcat(buf, "  '"); gsh_strcat(buf, line);
        gsh_strcat(buf, "': command not found");
        gsh_puts(buf, COL_ERROR);
    }
}

/* ═══════════════════════════ Entry point ══════════════════════════ */

int main(void)
{
    ik_gfx_init();
    ik_getcwd(cwd, 64);

    /* ── Banner ── */
    gsh_puts("", 0);
    gsh_puts("  IronKernel GUI Shell", COL_HEAD);
    gsh_puts("  ─────────────────────────────────────────", COL_DIM);
    gsh_puts("  Type 'help' for commands.  UP/DOWN scrolls.", COL_TEXT);
    gsh_puts("  Type 'exit' to close this window.", COL_DIM);
    gsh_puts("", 0);
    gsh_render();

    /* ── Main loop ── */
    for (;;) {
        gsh_getline();

        /* Check for exit before dispatching */
        if (gsh_strncmp(inp, "exit", 4)==0 &&
            (inp_len==4 || (inp_len>4 && gsh_isspace(inp[4]))))
            break;

        dispatch(inp);
        gsh_render();
    }

    return 0;
}
