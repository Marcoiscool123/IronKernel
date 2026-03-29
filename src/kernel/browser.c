/* ─────────────────────────────────────────────────────────────────────────
 * browser.c — Text-mode HTML browser for IronKernel
 *
 * Fetches pages via HTTP/1.0, strips HTML tags, and renders the text in a
 * full-width WM window.  Supports:
 *   - Tag stripping with block-element formatting (h1-h3, p, li, br)
 *   - HTML entity decoding (&lt; &gt; &amp; &nbsp; &#NNN;)
 *   - Word-wrap to window width
 *   - <a href> links rendered in blue; left-click follows them
 *   - PgUp / PgDn scrolling (handled automatically by the WM)
 *   - Close button exits; browser runs as a background WM shell task
 *
 * Usage: browser [http://]host[/path]
 * ──────────────────────────────────────────────────────────────────────── */

#include "types.h"
#include "wm.h"
#include "../drivers/vga.h"
#include "browser.h"
#include "../drivers/tcp.h"
#include "../drivers/dns.h"
#include "../drivers/mouse.h"

/* ── Window geometry (matches wm.c constants) ───────────────────────────── */
#define BR_X         6
#define BR_Y        28
#define BR_W       790    /* almost full screen width (800 - 10px margins) */
#define BR_H       546
#define BR_CW        8    /* char cell width  (CW  in wm.c) */
#define BR_CH        9    /* char cell height (CH  in wm.c) */
#define BR_TITLE_H  18    /* title bar height */
#define BR_BORDER    1    /* border width     */

/* ── Colour palette ─────────────────────────────────────────────────────── */
#define BR_C_BODY   0xC8D4E8u   /* body text  (= C_WIN_TXT in wm.c)   */
#define BR_C_LINK   0x60B0FFu   /* blue hyperlinks                     */
#define BR_C_H1     0xFFFFFFu   /* white h1                            */
#define BR_C_H2     0xE0E8FFu   /* near-white h2                       */
#define BR_C_H3     0xA0C0E0u   /* blue-grey h3                        */
#define BR_C_CODE   0x80FF90u   /* green monospace code                */
#define BR_C_ERR    0xFF5050u   /* red errors / status                 */
#define BR_C_OK     0x60DD60u   /* green ok status                     */
#define BR_C_DIM    0x708090u   /* dimmed / de-emphasised text         */

/* ── Link table ─────────────────────────────────────────────────────────── */
#define BR_MAX_LINKS 48

typedef struct {
    int  row;          /* buffer row where link text starts  */
    int  col;          /* buffer col where link text starts  */
    int  end_col;      /* buffer col where this row's portion ends */
    char url[256];     /* absolute URL (resolved at parse time)    */
} br_link_t;

/* ── Module state ───────────────────────────────────────────────────────── */
static int        br_win   = -1;
static br_link_t  br_links[BR_MAX_LINKS];
static int        br_nlinks;

/* Origin of the current page (for relative href resolution) */
static char  br_host[128];
static int   br_port;
static char  br_path[256];

/* Shared HTTP response buffer */
static uint8_t br_buf[8192];

/* ── Word-wrap state ─────────────────────────────────────────────────────── */
static char     br_word[256];   /* accumulated word (not yet emitted) */
static int      br_wlen;
static uint32_t br_wcolor;      /* colour of accumulated word         */
static int      br_col;         /* current text column (for wrap)     */

/* ── Active-link state ──────────────────────────────────────────────────── */
static int  br_in_link;
static int  br_link_start_row;
static int  br_link_start_col;
static char br_link_href[256];

/* ═══════════════════════════════════════════════════════════════════════════
 * String / utility helpers (no libc)
 * ══════════════════════════════════════════════════════════════════════════*/

static int br_slen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static void br_scpy(char *d, const char *s, int max)
{
    int i;
    for (i = 0; s[i] && i < max - 1; i++) d[i] = s[i];
    d[i] = '\0';
}

static char br_lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive tag-name check.
   tag = raw content between '<' and '>' (no leading '<').
   name = lowercase expected name (e.g. "br", "a", "h1"). */
static int br_tag_is(const char *tag, const char *name)
{
    int i = 0;
    while (name[i]) {
        if (br_lc(tag[i]) != name[i]) return 0;
        i++;
    }
    char c = tag[i];
    return !c || c == ' ' || c == '/' || c == '>' || c == '\t' || c == '\n';
}

/* Extract an attribute value from raw tag content.
   attr = lowercase attribute name.  out receives the value, up to maxlen-1 chars.
   Returns 1 if found, 0 if not. */
static int br_get_attr(const char *tag, const char *attr, char *out, int maxlen)
{
    int alen = br_slen(attr);
    int tlen = br_slen(tag);
    for (int i = 0; i + alen < tlen; i++) {
        /* match attribute name */
        int ok = 1;
        for (int j = 0; j < alen; j++)
            if (br_lc(tag[i+j]) != attr[j]) { ok = 0; break; }
        if (!ok) continue;
        /* must be followed by '=' (possibly with spaces) */
        int k = i + alen;
        while (tag[k] == ' ' || tag[k] == '\t') k++;
        if (tag[k] != '=') continue;
        k++;
        while (tag[k] == ' ' || tag[k] == '\t') k++;
        /* quoted or unquoted value */
        char q = 0;
        if (tag[k] == '"' || tag[k] == '\'') q = tag[k++];
        int n = 0;
        while (tag[k] && n < maxlen - 1) {
            if (q  && tag[k] == q) break;
            if (!q && (tag[k] == ' ' || tag[k] == '\t' || tag[k] == '>' || tag[k] == '/')) break;
            out[n++] = tag[k++];
        }
        out[n] = '\0';
        return 1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * URL utilities
 * ══════════════════════════════════════════════════════════════════════════*/

/* Parse [http[s]://]host[:port][/path] into components.
   Returns 0 on success, -1 on parse error. */
static int br_parse_url(const char *url,
                        char *host, int hmax,
                        int  *port,
                        char *path, int pmax)
{
    const char *p = url;
    /* Strip http:// or https:// */
    if (p[0]=='h' && p[1]=='t' && p[2]=='t' && p[3]=='p') {
        p += 4;
        if (*p == 's') p++;    /* https */
        if (p[0]==':' && p[1]=='/' && p[2]=='/') p += 3;
        else return -1;
    }
    /* Host */
    int hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < hmax - 1)
        host[hi++] = *p++;
    host[hi] = '\0';
    if (!hi) return -1;
    /* Optional port */
    *port = 80;
    if (*p == ':') {
        p++;
        int pv = 0;
        while (*p >= '0' && *p <= '9') pv = pv * 10 + (*p++ - '0');
        if (pv) *port = pv;
    }
    /* Path */
    if (*p == '/') {
        int pi = 0;
        while (*p && pi < pmax - 1) path[pi++] = *p++;
        path[pi] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }
    return 0;
}

/* Resolve href relative to current page origin.
   Writes absolute URL into out (up to maxlen-1 chars). */
static void br_resolve_href(const char *href, char *out, int maxlen)
{
    /* Already absolute */
    if (href[0]=='h' && href[1]=='t' && href[2]=='t' && href[3]=='p') {
        br_scpy(out, href, maxlen); return;
    }
    /* Protocol-relative  //host/... */
    if (href[0]=='/' && href[1]=='/') {
        out[0]='h'; out[1]='t'; out[2]='t'; out[3]='p'; out[4]=':';
        br_scpy(out + 5, href, maxlen - 5); return;
    }
    /* Build "http://host[:port]" prefix */
    char pre[160];
    int  pi = 0;
    const char *sch = "http://";
    for (; *sch; sch++) pre[pi++] = *sch;
    for (int i = 0; br_host[i] && pi < 140; i++) pre[pi++] = br_host[i];
    if (br_port != 80) {
        pre[pi++] = ':';
        int tmp = br_port, d = 0; char db[8];
        if (!tmp) { db[0]='0'; d=1; }
        else { while (tmp) { db[d++]='0'+(tmp%10); tmp/=10; } }
        for (int i=d-1; i>=0; i--) pre[pi++] = db[i];
    }
    pre[pi] = '\0';

    if (href[0] == '/') {
        /* Site-relative */
        br_scpy(out, pre, maxlen);
        int n = br_slen(out);
        br_scpy(out + n, href, maxlen - n);
        return;
    }
    /* Page-relative: strip to last '/' in current path */
    char base[256];
    br_scpy(base, br_path, 256);
    int last = 0;
    for (int i = 0; base[i]; i++) if (base[i] == '/') last = i + 1;
    base[last] = '\0';

    br_scpy(out, pre, maxlen);
    int n = br_slen(out);
    br_scpy(out + n, base, maxlen - n);
    n += br_slen(base);
    br_scpy(out + n, href, maxlen - n);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Word-wrap output helpers
 * ══════════════════════════════════════════════════════════════════════════*/

/* Flush accumulated word to window with wrapping. */
static void br_flush_word(void)
{
    if (!br_wlen) return;
    wm_win_t *wp = &wm_wins[br_win];
    int vis = wp->vis_cols ? wp->vis_cols : 80;

    if (br_col + br_wlen > vis && br_col > 0) {
        wm_putchar(br_win, '\n');
        br_col = 0;
    }
    uint32_t sv = wm_cur_fg;
    wm_cur_fg = br_wcolor;
    for (int i = 0; i < br_wlen; i++) {
        wm_putchar(br_win, br_word[i]);
        br_col++;
    }
    wm_cur_fg = sv;
    br_wlen = 0;
}

/* Add char to word buffer; color change forces a flush first. */
static void br_add(char c, uint32_t color)
{
    if (!br_wlen) { br_wcolor = color; }
    else if (br_wcolor != color) { br_flush_word(); br_wcolor = color; }
    if (br_wlen < 255) br_word[br_wlen++] = c;
}

/* Emit a space (flush word first; suppress leading spaces). */
static void br_space(void)
{
    br_flush_word();
    if (br_col > 0) {
        wm_cur_fg = BR_C_BODY;
        wm_putchar(br_win, ' ');
        br_col++;
    }
}

/* Emit a newline. */
static void br_newline(void)
{
    br_flush_word();
    wm_putchar(br_win, '\n');
    br_col = 0;
}

/* Emit a blank separator line (skip if already on a blank line). */
static void br_blank(void)
{
    br_flush_word();
    wm_win_t *wp = &wm_wins[br_win];
    /* Check if current row is already blank */
    int r = wp->cur_row;
    if (wp->cur_col == 0 && r > 0) {
        /* Check previous row for content */
        int prev = r - 1;
        int empty = 1;
        for (int c = 0; c < WM_BUF_COLS; c++)
            if (wp->buf[prev][c] != ' ') { empty = 0; break; }
        if (empty) return;   /* already blank above — don't double-blank */
    }
    if (wp->cur_col > 0) wm_putchar(br_win, '\n');
    wm_putchar(br_win, '\n');
    br_col = 0;
}

/* Emit a heading line: blank before, text in color, blank after. */
static void br_heading(const char *text, uint32_t color)
{
    br_blank();
    uint32_t sv = wm_cur_fg;
    wm_cur_fg = color;
    wm_puts(br_win, text);
    wm_cur_fg = sv;
    br_col = br_slen(text);
    br_newline();
    br_blank();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HTML entity decoder
 * ══════════════════════════════════════════════════════════════════════════*/

/* Decode entity text (the part between '&' and ';').
   Returns the decoded char, or '?' if unknown. */
static char br_entity(const char *e, int n)
{
    if (!n) return '?';
    /* Numeric: &#NNN; or &#xHH; */
    if (e[0] == '#') {
        int v = 0;
        if (n > 1 && (e[1]=='x'||e[1]=='X')) {
            for (int i=2; i<n; i++) {
                char c = e[i];
                if      (c>='0'&&c<='9') v=v*16+(c-'0');
                else if (c>='a'&&c<='f') v=v*16+(c-'a'+10);
                else if (c>='A'&&c<='F') v=v*16+(c-'A'+10);
            }
        } else {
            for (int i=1; i<n; i++)
                if (e[i]>='0'&&e[i]<='9') v=v*10+(e[i]-'0');
        }
        return (v>0 && v<128) ? (char)v : '?';
    }
    /* Named */
    static const struct { const char *nm; char ch; } tbl[] = {
        {"lt",'<'},{"gt",'>'},{"amp",'&'},{"quot",'"'},{"apos",'\''},
        {"nbsp",' '},{"mdash",'-'},{"ndash",'-'},{"laquo",'<'},
        {"raquo",'>'},{"copy",'c'},{"trade",'t'},{"hellip",'.'},
        {0,0}
    };
    for (int i=0; tbl[i].nm; i++) {
        const char *nm = tbl[i].nm;
        int j=0;
        while (nm[j] && j<n && br_lc(e[j])==nm[j]) j++;
        if (!nm[j] && j==n) return tbl[i].ch;
    }
    return '?';
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HTML renderer
 * ══════════════════════════════════════════════════════════════════════════*/

/* Process a complete tag (text between '<' and '>').
   tag[0] is the first char of the tag name (or '/' for closing tags).
   pre_mode → whitespace is preserved (inside <pre>). */
static void br_handle_tag(const char *tag, int *pre_mode,
                          int *skip_depth, const char *skip_tag)
{
    /* Closing tag? */
    int closing = (tag[0] == '/');
    const char *t = closing ? tag + 1 : tag;

    /* Skip mode: ignore everything until we see </skip_tag> */
    if (*skip_depth > 0) {
        if (closing && br_tag_is(t, skip_tag)) { (*skip_depth)--; }
        return;
    }

    /* Block tags that flush + newline */
    if (br_tag_is(t, "br") || br_tag_is(t, "br/")) {
        br_newline(); return;
    }
    if (br_tag_is(t, "p")) {
        if (!closing) br_blank(); else br_newline();
        return;
    }
    if (br_tag_is(t, "div") || br_tag_is(t, "section") ||
        br_tag_is(t, "article") || br_tag_is(t, "header") ||
        br_tag_is(t, "footer") || br_tag_is(t, "nav") ||
        br_tag_is(t, "main"))
    { br_newline(); return; }

    if (br_tag_is(t, "li") || br_tag_is(t, "dt")) {
        if (!closing) {
            br_newline();
            uint32_t sv = wm_cur_fg;
            wm_cur_fg = BR_C_DIM;
            wm_puts(br_win, "  * ");
            wm_cur_fg = sv;
            br_col = 4;
        }
        return;
    }
    if (br_tag_is(t, "hr")) {
        br_newline();
        uint32_t sv = wm_cur_fg;
        wm_cur_fg = BR_C_DIM;
        wm_puts(br_win, "────────────────────────────────────────────────────────────────────────────────────────────");
        wm_cur_fg = sv;
        br_newline();
        br_col = 0;
        return;
    }

    /* Headings */
    if (!closing) {
        if (br_tag_is(t, "h1")) {
            br_blank();
            wm_cur_fg = BR_C_H1;
            br_wcolor = BR_C_H1;
            br_col = 0;
            return;
        }
        if (br_tag_is(t, "h2")) {
            br_blank();
            wm_cur_fg = BR_C_H2;
            br_wcolor = BR_C_H2;
            br_col = 0;
            return;
        }
        if (br_tag_is(t, "h3") || br_tag_is(t, "h4") ||
            br_tag_is(t, "h5") || br_tag_is(t, "h6")) {
            br_blank();
            wm_cur_fg = BR_C_H3;
            br_wcolor = BR_C_H3;
            br_col = 0;
            return;
        }
    } else {
        if (br_tag_is(t, "h1") || br_tag_is(t, "h2") || br_tag_is(t, "h3") ||
            br_tag_is(t, "h4") || br_tag_is(t, "h5") || br_tag_is(t, "h6")) {
            br_flush_word();
            wm_cur_fg = BR_C_BODY;
            br_newline();
            br_blank();
            return;
        }
    }

    /* Code / pre */
    if (br_tag_is(t, "pre")) {
        *pre_mode = !closing;
        if (!closing) { br_blank(); wm_cur_fg = BR_C_CODE; br_wcolor = BR_C_CODE; }
        else          { br_flush_word(); wm_cur_fg = BR_C_BODY; br_blank(); }
        return;
    }
    if (br_tag_is(t, "code") || br_tag_is(t, "tt") || br_tag_is(t, "kbd")) {
        wm_cur_fg = closing ? BR_C_BODY : BR_C_CODE;
        br_wcolor = wm_cur_fg;
        return;
    }
    if (br_tag_is(t, "em") || br_tag_is(t, "i")) {
        wm_cur_fg = closing ? BR_C_BODY : BR_C_DIM;
        br_wcolor = wm_cur_fg;
        return;
    }
    if (br_tag_is(t, "strong") || br_tag_is(t, "b")) {
        /* Bold: use slightly brighter body color */
        wm_cur_fg = closing ? BR_C_BODY : 0xFFFFFFu;
        br_wcolor = wm_cur_fg;
        return;
    }

    /* Anchor / hyperlink */
    if (br_tag_is(t, "a") && !closing) {
        char href[256]; href[0] = '\0';
        br_get_attr(tag, "href", href, 256);
        if (href[0]) {
            br_flush_word();
            br_in_link = 1;
            br_resolve_href(href, br_link_href, 256);
            wm_win_t *wp = &wm_wins[br_win];
            br_link_start_row = wp->cur_row;
            br_link_start_col = wp->cur_col;
            wm_cur_fg = BR_C_LINK;
            br_wcolor = BR_C_LINK;
        }
        return;
    }
    if (br_tag_is(t, "a") && closing) {
        if (br_in_link && br_nlinks < BR_MAX_LINKS) {
            br_flush_word();
            wm_win_t *wp = &wm_wins[br_win];
            br_link_t *lk = &br_links[br_nlinks++];
            lk->row     = br_link_start_row;
            lk->col     = br_link_start_col;
            lk->end_col = wp->cur_col;
            br_scpy(lk->url, br_link_href, 256);
        }
        br_in_link = 0;
        wm_cur_fg = BR_C_BODY;
        br_wcolor = BR_C_BODY;
        return;
    }

    /* Skip invisible blocks entirely */
    if (br_tag_is(t, "script") || br_tag_is(t, "style") ||
        br_tag_is(t, "noscript") || br_tag_is(t, "svg") ||
        br_tag_is(t, "head"))
    {
        if (!closing) {
            /* Copy tag name into skip_tag static buffer — reuse first arg */
            *skip_depth = 1;
        }
        return;
    }
}

/* Render HTML body bytes into the browser window.
   html[0..len-1] is the raw response body. */
static void html_render(const uint8_t *html, int len)
{
    /* ── Reset render state ── */
    br_wlen     = 0;
    br_col      = 0;
    br_wcolor   = BR_C_BODY;
    wm_cur_fg   = BR_C_BODY;
    br_in_link  = 0;

    /* ── Parser state ── */
    int pre_mode   = 0;   /* inside <pre>       */
    int skip_depth = 0;   /* inside script/style/head */
    char skip_tag[16]; skip_tag[0] = '\0';

    /* Buffers for accumulating tags and entities */
    char  tagbuf[1024];  int tlen = 0;
    char  entbuf[16];    int elen = 0;
    int   cm = 0;        /* --> detector for ST_COMMENT */

    typedef enum { ST_TEXT, ST_TAG, ST_COMMENT, ST_ENTITY } St;
    St state = ST_TEXT;

    /* Whitespace collapse: track if last emitted was a space */
    int last_space = 1;   /* start as 1 to suppress leading spaces */

    for (int i = 0; i < len; i++) {
        char c = (char)html[i];

        switch (state) {

        /* ── Normal text ── */
        case ST_TEXT:
            if (c == '<') {
                /* Flush pending space before tag */
                tlen = 0;
                state = ST_TAG;
                break;
            }
            if (c == '&') {
                elen = 0;
                state = ST_ENTITY;
                break;
            }
            if (skip_depth > 0) break;  /* skip text inside script/style */

            /* Whitespace handling */
            if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
                if (pre_mode) {
                    if (c == '\n') { br_flush_word(); wm_putchar(br_win,'\n'); br_col=0; }
                    else if (c=='\t') { br_add(' ',wm_cur_fg); br_add(' ',wm_cur_fg);
                                        br_add(' ',wm_cur_fg); br_add(' ',wm_cur_fg); }
                    else br_add(' ', wm_cur_fg);
                } else if (!last_space) {
                    /* Emit one space (lazy — becomes word separator on flush) */
                    if (br_wlen > 0) {
                        /* Flush the word then space */
                        br_flush_word();
                        if (br_col > 0) {
                            wm_cur_fg = BR_C_BODY;
                            wm_putchar(br_win,' '); br_col++;
                        }
                        last_space = 1;
                    } else {
                        last_space = 1;  /* swallow space at line start */
                    }
                }
            } else {
                last_space = 0;
                br_add(c, wm_cur_fg);
            }
            break;

        /* ── Inside <...> ── */
        case ST_TAG:
            if (tlen == 0 && c == '!') {
                /* Might be comment */
                tagbuf[tlen++] = c;
                break;
            }
            if (tlen == 1 && tagbuf[0] == '!') {
                /* First char after '!': if '-', accumulate and wait for second */
                if (c == '-') { tagbuf[tlen++] = c; }
                if (c == '>') state = ST_TEXT;
                break;
            }
            if (tlen == 2 && tagbuf[0] == '!' && tagbuf[1] == '-') {
                /* Second char: if '-', enter comment state */
                if (c == '-') { state = ST_COMMENT; tlen = 0; }
                else if (c == '>') { state = ST_TEXT; }
                /* else: <!-x... treated as unknown, keep accumulating */
                break;
            }
            if (c == '>') {
                tagbuf[tlen] = '\0';
                /* Detect skip-tag name for script/style/head */
                const char *skip_names[] = {"script","style","noscript","svg","head",0};
                int is_open_skip = 0;
                for (int si=0; skip_names[si]; si++) {
                    if (br_tag_is(tagbuf, skip_names[si])) {
                        is_open_skip = 1;
                        br_scpy(skip_tag, skip_names[si], 16);
                        break;
                    }
                }
                /* Check if it's a closing of the current skip tag */
                if (skip_depth > 0 && tagbuf[0]=='/' &&
                    br_tag_is(tagbuf+1, skip_tag)) {
                    skip_depth = 0;
                } else if (is_open_skip) {
                    skip_depth = 1;
                } else {
                    br_handle_tag(tagbuf, &pre_mode, &skip_depth, skip_tag);
                    /* After block tag, reset last_space */
                    last_space = (br_col == 0);
                }
                tlen = 0;
                state = ST_TEXT;
                break;
            }
            if (tlen < 1023) tagbuf[tlen++] = c;
            break;

        /* ── Inside <!-- ... --> ── */
        case ST_COMMENT: {
            /* Scan for --> */
            if (c == '-') cm++;
            else if (c == '>' && cm >= 2) { state = ST_TEXT; cm = 0; }
            else cm = 0;
            break;
        }

        /* ── Inside &...; ── */
        case ST_ENTITY:
            if (c == ';') {
                entbuf[elen] = '\0';
                if (skip_depth == 0) {
                    char ec = br_entity(entbuf, elen);
                    if (ec == ' ') {
                        br_flush_word();
                        if (br_col > 0) { wm_putchar(br_win,' '); br_col++; }
                        last_space = 1;
                    } else if (ec != '?') {
                        br_add(ec, wm_cur_fg);
                        last_space = 0;
                    }
                }
                elen = 0;
                state = ST_TEXT;
            } else if (c == '<' || c == ' ') {
                /* Malformed entity — emit literal & and the chars */
                if (skip_depth == 0) br_add('&', wm_cur_fg);
                for (int j=0; j<elen && skip_depth==0; j++) br_add(entbuf[j], wm_cur_fg);
                elen = 0;
                state = (c == '<') ? ST_TAG : ST_TEXT;
                if (c == '<') tlen = 0;
            } else {
                if (elen < 15) entbuf[elen++] = c;
            }
            break;
        }
    }
    /* Flush any remaining word */
    br_flush_word();
    br_newline();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Click detection
 * ══════════════════════════════════════════════════════════════════════════*/

/* Convert mouse pixel coordinates to buffer row/col accounting for scroll.
   Returns link index if click lands on a link, else -1. */
static int br_hit_link(int mx, int my)
{
    wm_win_t *wp = &wm_wins[br_win];
    /* Window client area origin */
    int cx = wp->x + BR_BORDER;
    int cy = wp->y + BR_TITLE_H + BR_BORDER;
    /* Click relative to client area */
    int rx = mx - cx;
    int ry = my - cy;
    if (rx < 0 || ry < 0) return -1;
    int screen_row = ry / BR_CH;
    int screen_col = rx / BR_CW;
    if (screen_row >= wp->vis_rows || screen_col >= wp->vis_cols) return -1;
    int buf_row = wp->top_row + screen_row;

    for (int i = 0; i < br_nlinks; i++) {
        br_link_t *lk = &br_links[i];
        if (lk->row != buf_row) continue;
        /* Link is on this row — check column range (end_col=0 means unknown) */
        if (lk->end_col > 0) {
            if (screen_col >= lk->col && screen_col < lk->end_col) return i;
        } else {
            /* Fallback: click anywhere on the link row */
            if (screen_col >= lk->col) return i;
        }
    }
    return -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Navigation — fetch URL and render into browser window
 * ══════════════════════════════════════════════════════════════════════════*/

/* Update the window title to show the current host. */
static void br_set_title(const char *host)
{
    wm_win_t *wp = &wm_wins[br_win];
    const char *pfx = "Browser — ";
    int i = 0;
    for (; *pfx && i < 30; pfx++, i++) wp->title[i] = *pfx;
    for (int j = 0; host[j] && i < 30; j++, i++) wp->title[i] = host[j];
    wp->title[i] = '\0';
    wp->dirty = 1;
}

/* Clear the browser window and reset render state. */
static void br_clear(void)
{
    wm_win_t *wp = &wm_wins[br_win];
    for (int r = 0; r < WM_BUF_ROWS; r++)
        for (int c = 0; c < WM_BUF_COLS; c++) {
            wp->buf[r][c] = ' ';
            wp->col[r][c] = BR_C_BODY;
        }
    wp->cur_col = 0;
    wp->cur_row = 0;
    wp->top_row = 0;
    wp->dirty   = 1;
    br_nlinks   = 0;
    br_wlen     = 0;
    br_col      = 0;
    br_in_link  = 0;
}

/* Fetch and render a URL into the browser window.
   url = "http://host[:port]/path" or "host[/path]" */
static void br_navigate(const char *url)
{
    char host[128], path[256];
    int  port = 80;

    if (br_parse_url(url, host, 128, &port, path, 256) < 0) {
        br_clear();
        uint32_t sv = wm_cur_fg;
        wm_cur_fg = BR_C_ERR;
        wm_puts(br_win, " Invalid URL: ");
        wm_puts(br_win, url);
        wm_puts(br_win, "\n");
        wm_cur_fg = sv;
        wm_wins[br_win].dirty = 1;
        return;
    }

    /* Save as current page origin */
    br_scpy(br_host, host, 128);
    br_port = port;
    br_scpy(br_path, path, 256);

    br_clear();
    br_set_title(host);

    /* Show loading status */
    {
        uint32_t sv = wm_cur_fg;
        wm_cur_fg = BR_C_DIM;
        wm_puts(br_win, " [1] Resolving ");
        wm_puts(br_win, host);
        wm_puts(br_win, "...\n");
        wm_cur_fg = sv;
        wm_wins[br_win].dirty = 1;
    }

    /* DNS resolve */
    uint8_t ip[4];
    if (dns_resolve(host, ip) < 0) {
        uint32_t sv = wm_cur_fg;
        wm_cur_fg = BR_C_ERR;
        wm_puts(br_win, " DNS lookup failed: ");
        wm_puts(br_win, host);
        wm_puts(br_win, "\n");
        wm_cur_fg = sv;
        wm_wins[br_win].dirty = 1;
        return;
    }

    wm_puts(br_win, " [2] Connecting...\n");
    wm_wins[br_win].dirty = 1;

    /* HTTP GET */
    int n = http_get(ip, (uint16_t)port, host, path, br_buf, sizeof(br_buf) - 1);
    if (n < 0) {
        uint32_t sv = wm_cur_fg;
        wm_cur_fg = BR_C_ERR;
        wm_puts(br_win, " Connection failed.\n");
        wm_cur_fg = sv;
        wm_wins[br_win].dirty = 1;
        return;
    }
    br_buf[n] = '\0';

    wm_puts(br_win, " [3] Rendering...\n");
    wm_wins[br_win].dirty = 1;

    br_clear();
    br_set_title(host);

    /* Render HTML body */
    html_render(br_buf, n);
    wm_wins[br_win].dirty = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Entry point
 * ══════════════════════════════════════════════════════════════════════════*/

void cmd_browser(const char *args)
{
    if (!wm_is_running) {
        vga_print("  browser: requires GUI mode (run 'gui' first)\n");
        return;
    }
    while (*args == ' ') args++;

    /* Close previous browser window if still open */
    if (br_win >= 0 && br_win < WM_MAX_WIN && wm_wins[br_win].alive)
        wm_wins[br_win].alive = 0;

    /* Create browser window */
    br_win = wm_create(BR_X, BR_Y, BR_W, BR_H, "Browser");
    if (br_win < 0) return;

    wm_focused = br_win;   /* keyboard focus to browser */
    wm_z_top   = br_win;   /* bring browser window to visual front */

    /* Navigate to the requested URL (or show a welcome page) */
    if (*args) {
        br_navigate(args);
    } else {
        br_clear();
        br_set_title("about:start");
        uint32_t sv = wm_cur_fg;
        wm_cur_fg = BR_C_H1;
        wm_puts(br_win, " IronKernel Browser\n\n");
        wm_cur_fg = BR_C_BODY;
        wm_puts(br_win, " Usage: browser http://neverssl.com/\n");
        wm_puts(br_win, " Usage: browser neverssl.com\n\n");
        wm_puts(br_win, " Click a highlighted link to follow it.\n");
        wm_puts(br_win, " PgUp / PgDn to scroll.\n");
        wm_puts(br_win, " Close button to exit.\n");
        wm_cur_fg = sv;
        wm_wins[br_win].dirty = 1;
    }

    /* ── Event loop: handle link clicks, detect window close ── */
    uint8_t prev_btn = 0;
    while (br_win >= 0 && *(volatile int*)&wm_wins[br_win].alive) {
        uint8_t cur_btn = mouse_btn;

        /* Left-click edge detection */
        if ((cur_btn & 0x01) && !(prev_btn & 0x01)) {
            if (wm_focused == br_win) {
                int li = br_hit_link(mouse_x, mouse_y);
                if (li >= 0) {
                    br_navigate(br_links[li].url);
                }
            }
        }
        prev_btn = cur_btn;

        __asm__ volatile("pause");
    }
}
