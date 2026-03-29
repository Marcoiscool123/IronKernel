/* IronKernel v0.04 — gdt.h */
#ifndef GDT_H
#define GDT_H
#include "types.h"

/* GDT selector values (index << 3 | TI | RPL) */
#define GDT_KERNEL_CODE  0x08   /* index 1, DPL 0 */
#define GDT_KERNEL_DATA  0x10   /* index 2, DPL 0 */
#define GDT_USER_CODE    0x1B   /* index 3, DPL 3  (0x18 | 3) */
#define GDT_USER_DATA    0x23   /* index 4, DPL 3  (0x20 | 3) */
#define GDT_TSS_SEL      0x28   /* index 5, DPL 0 */

/* 80-bit GDTR — base must be 64-bit in long mode */
typedef struct __attribute__((packed)) {
    uint16_t limit;   /* byte size of GDT minus 1 */
    uint64_t base;    /* linear address of GDT     */
} gdt_ptr64_t;

void gdt_init(void);

/* Implemented in boot/gdt.asm */
void gdt_flush(gdt_ptr64_t *gdtr);
void tss_flush(uint16_t sel);

#endif
