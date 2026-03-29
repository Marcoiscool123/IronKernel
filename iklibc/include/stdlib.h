/* IronKernel iklibc — stdlib.h */
#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

/* Program termination */
__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _Exit(int status);

/* Number conversion */
int        atoi (const char *s);
long       atol (const char *s);
long long  atoll(const char *s);

/* Non-standard but useful in bare-metal contexts */
char* itoa(int n,                char *buf, int base);
char* utoa(unsigned long long n, char *buf, int base);

/* Math */
int  abs(int n);
long labs(long n);

/* Memory — simple bump allocator backed by a 64 KB static pool.
   free() is a no-op; memory cannot be reclaimed. */
void* malloc(size_t size);
void  free(void *ptr);

#endif
