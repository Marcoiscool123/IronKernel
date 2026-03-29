/* IronKernel iklibc — stddef.h */
#ifndef STDDEF_H
#define STDDEF_H

typedef unsigned long long size_t;
typedef long long          ptrdiff_t;

#define NULL ((void*)0)
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
