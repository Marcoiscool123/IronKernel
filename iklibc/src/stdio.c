/* IronKernel iklibc — stdio.c
   printf, puts, putchar, gets_s.
   Output is buffered into a 1 KB stack buffer then flushed via ik_write.
   Supported specifiers: %[-][0][width][ll]s c d i u x X p %% */
#include <stdio.h>
#include <stdint.h>
#include <ironkernel.h>

/* ── Internal helpers ──────────────────────────────────────────────── */

#define PRINTF_BUF 1024

typedef struct {
    char   buf[PRINTF_BUF];
    int    pos;
} pbuf_t;

static void pb_char(pbuf_t *b, char c)
{
    if (b->pos < PRINTF_BUF - 1)
        b->buf[b->pos++] = c;
}

static void pb_str(pbuf_t *b, const char *s)
{
    if (!s) s = "(null)";
    while (*s) pb_char(b, *s++);
}

static void pb_flush(pbuf_t *b)
{
    b->buf[b->pos] = '\0';
    ik_write(b->buf);
    b->pos = 0;
}

/* Convert unsigned integer to string in tmp[]; return pointer into tmp[]. */
static char *fmt_uint(char *tmp, int tmpsz, unsigned long long n, int base, int upper)
{
    const char *lo = "0123456789abcdef";
    const char *up = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;
    int i = tmpsz - 1;
    tmp[i] = '\0';
    if (n == 0) { tmp[--i] = '0'; return &tmp[i]; }
    while (n > 0) { tmp[--i] = digits[n % (unsigned)base]; n /= (unsigned)base; }
    return &tmp[i];
}

/* Write string s with optional width padding. */
static void pb_field(pbuf_t *b, const char *s, int width, int left, char pad)
{
    int len = 0;
    const char *p = s;
    while (*p) { len++; p++; }
    if (!left) { for (int i = len; i < width; i++) pb_char(b, pad); }
    pb_str(b, s);
    if ( left) { for (int i = len; i < width; i++) pb_char(b, ' '); }
}

/* ── printf ────────────────────────────────────────────────────────── */

int printf(const char *fmt, ...)
{
    pbuf_t b;
    b.pos = 0;

    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') { pb_char(&b, *fmt++); continue; }
        fmt++; /* skip '%' */

        /* ── Flags ── */
        int flag_left = 0, flag_zero = 0;
        for (;;) {
            if (*fmt == '-')      { flag_left = 1; fmt++; }
            else if (*fmt == '0') { flag_zero = 1; fmt++; }
            else if (*fmt == '+' || *fmt == ' ') { fmt++; } /* ignore */
            else break;
        }

        /* ── Width ── */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        /* ── Precision (skip) ── */
        if (*fmt == '.') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') fmt++;
        }

        /* ── Length modifier ── */
        int is_ll = 0;
        if (fmt[0] == 'l' && fmt[1] == 'l') { is_ll = 1; fmt += 2; }
        else if (fmt[0] == 'l')              { is_ll = 1; fmt += 1; }

        char pad = (flag_zero && !flag_left) ? '0' : ' ';
        char tmp[72]; /* enough for 64-bit numbers */

        switch (*fmt++) {
        case 's': {
            const char *sv = __builtin_va_arg(ap, const char *);
            pb_field(&b, sv ? sv : "(null)", width, flag_left, ' ');
            break;
        }
        case 'c': {
            char cv[2] = { (char)__builtin_va_arg(ap, int), '\0' };
            pb_field(&b, cv, width, flag_left, ' ');
            break;
        }
        case 'd': case 'i': {
            long long iv = is_ll ? __builtin_va_arg(ap, long long)
                                 : (long long)__builtin_va_arg(ap, int);
            int neg = (iv < 0);
            unsigned long long uv = neg ? (unsigned long long)-iv : (unsigned long long)iv;
            char *s = fmt_uint(tmp, sizeof(tmp), uv, 10, 0);
            /* For zero-pad with sign, prepend '-' before padding */
            if (neg && flag_zero && !flag_left && width > 0) {
                pb_char(&b, '-');
                width--;
                pb_field(&b, s, width, 0, '0');
            } else if (neg) {
                /* Build "-NNN" in tmp and let pb_field pad with spaces */
                char tmp2[72]; tmp2[0] = '-';
                int slen = 0; char *p = s; while (*p) { tmp2[1+slen++] = *p++; }
                tmp2[1+slen] = '\0';
                pb_field(&b, tmp2, width, flag_left, ' ');
            } else {
                pb_field(&b, s, width, flag_left, pad);
            }
            break;
        }
        case 'u': {
            unsigned long long uv = is_ll ? __builtin_va_arg(ap, unsigned long long)
                                          : (unsigned long long)__builtin_va_arg(ap, unsigned int);
            pb_field(&b, fmt_uint(tmp, sizeof(tmp), uv, 10, 0), width, flag_left, pad);
            break;
        }
        case 'x': {
            unsigned long long uv = is_ll ? __builtin_va_arg(ap, unsigned long long)
                                          : (unsigned long long)__builtin_va_arg(ap, unsigned int);
            pb_field(&b, fmt_uint(tmp, sizeof(tmp), uv, 16, 0), width, flag_left, pad);
            break;
        }
        case 'X': {
            unsigned long long uv = is_ll ? __builtin_va_arg(ap, unsigned long long)
                                          : (unsigned long long)__builtin_va_arg(ap, unsigned int);
            pb_field(&b, fmt_uint(tmp, sizeof(tmp), uv, 16, 1), width, flag_left, pad);
            break;
        }
        case 'p':
            pb_str(&b, "0x");
            pb_field(&b, fmt_uint(tmp, sizeof(tmp),
                      (unsigned long long)__builtin_va_arg(ap, void *), 16, 0),
                      width, flag_left, pad);
            break;
        case '%':
            pb_char(&b, '%');
            break;
        default:
            pb_char(&b, '%');
            break;
        }
    }

    __builtin_va_end(ap);
    pb_flush(&b);
    return b.pos;
}

/* ── puts / putchar / gets_s ───────────────────────────────────────── */

int puts(const char *s)
{
    ik_write(s);
    ik_write("\n");
    return 0;
}

int putchar(int c)
{
    char s[2] = { (char)c, '\0' };
    ik_write(s);
    return c;
}

/* Reads one line of keyboard input into buf (max bufsz-1 chars + NUL).
   Returns 0 on success. */
int gets_s(char *buf, size_t bufsz)
{
    ik_read(buf, (uint64_t)bufsz);
    return 0;
}
