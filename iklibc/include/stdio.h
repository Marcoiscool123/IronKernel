/* IronKernel iklibc — stdio.h
   No FILE*, no buffering infrastructure.
   All output goes through SYS_WRITE. */
#ifndef STDIO_H
#define STDIO_H

#include <stddef.h>

int   printf(const char *fmt, ...);
int   puts(const char *s);
int   putchar(int c);
int   gets_s(char *buf, size_t bufsz);

#endif
