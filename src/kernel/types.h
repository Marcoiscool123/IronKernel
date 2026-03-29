/* IronKernel v0.04 — types.h
   In 64-bit long mode, pointers are 8 bytes.
   uintptr_t and size_t must widen to uint64_t or pointer casts truncate. */
#ifndef TYPES_H
#define TYPES_H

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

typedef uint64_t  uintptr_t;
/* Pointer-sized integer. Now 64-bit. Used to cast pointers for arithmetic.
   Example: VGA buffer at 0xB8000 = (volatile uint16_t*)(uintptr_t)0xB8000 */

typedef uint64_t  size_t;
/* Size/count type. Widened to 64-bit for correctness in long mode.
   kmalloc(size_t n), memset(..., size_t n) etc. all use this. */

/* bool/true/false omitted — GCC provides these as keywords in C11 */
#define NULL   ((void*)0)

#endif
