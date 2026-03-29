/* IronKernel iklibc — stdlib.c */
#include <stdlib.h>
#include <stdint.h>
#include <ironkernel.h>

/* ── exit / _Exit ──────────────────────────────────────────────────── */

__attribute__((noreturn))
void exit(int status)
{
    ik_exit(status);
}

__attribute__((noreturn))
void _Exit(int status)
{
    exit(status);
}

/* ── Number parsing ────────────────────────────────────────────────── */

long long atoll(const char *s)
{
    long long n = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if      (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}

long atol(const char *s)  { return (long)atoll(s); }
int  atoi(const char *s)  { return (int) atoll(s); }

/* ── Number formatting ─────────────────────────────────────────────── */

char *utoa(unsigned long long n, char *buf, int base)
{
    const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[66]; int i = 65; tmp[65] = '\0';
    if (base < 2 || base > 36) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return buf; }
    while (n > 0) {
        tmp[--i] = digits[n % (unsigned)base];
        n /= (unsigned)base;
    }
    int len = 65 - i;
    for (int j = 0; j <= len; j++) buf[j] = tmp[i + j];
    return buf;
}

char *itoa(int n, char *buf, int base)
{
    if (base == 10 && n < 0) {
        buf[0] = '-';
        utoa((unsigned long long)-(long long)n, buf + 1, base);
    } else {
        utoa((unsigned long long)(unsigned int)n, buf, base);
    }
    return buf;
}

/* ── Math ──────────────────────────────────────────────────────────── */

int  abs (int  n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }

/* ── malloc / free — bump allocator ───────────────────────────────── */

/* 64 KB static heap in .bss (zeroed by crt0.asm). */
static uint8_t  _heap[65536];
static size_t   _heap_pos = 0;

void *malloc(size_t size)
{
    /* 8-byte align every allocation. */
    size = (size + 7u) & ~7u;
    if (_heap_pos + size > sizeof(_heap)) return (void *)0;
    void *ptr = (void *)&_heap[_heap_pos];
    _heap_pos += size;
    return ptr;
}

void free(void *ptr)
{
    (void)ptr; /* Bump allocator — memory is never reclaimed. */
}
