/* IronKernel v0.04 — tss.h
   64-bit Task State Segment — Intel SDM Vol.3A §7.7
   The CPU reads rsp0 on every ring-3→ring-0 transition to find the
   kernel stack pointer. Without a valid TSS, user-mode interrupts
   load a garbage RSP and triple-fault. */
#ifndef TSS_H
#define TSS_H
#include "types.h"

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;      /* ring-0 stack pointer — loaded on interrupt from ring 3 */
    uint64_t rsp1;      /* ring-1 (unused) */
    uint64_t rsp2;      /* ring-2 (unused) */
    uint64_t reserved1;
    uint64_t ist[7];    /* Interrupt Stack Table — unused, all zero */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base; /* = sizeof(TSS64) → no I/O port bitmap */
} TSS64;

/* These selectors are used by usermode.asm and elf.c */
#define GDT_USER_CODE  0x1B   /* index 3 | RPL 3 */
#define GDT_USER_DATA  0x23   /* index 4 | RPL 3 */
#define GDT_TSS_SEL    0x28   /* index 5 | RPL 0 */

void    tss_init(uint64_t kernel_stack_top);
void    tss_set_rsp0(uint64_t rsp0);
TSS64  *tss_get(void);
uint32_t sizeof_tss(void);

/* Defined in usermode.asm — the ring-3 entry function */
void user_mode_enter(uint64_t entry, uint64_t user_stack_top);
/* kernel_exit_restore — restores all callee-saved regs, clears
   user_mode_active, restores kernel stack, and jumps back to
   the instruction after user_mode_enter. Never returns. */
void kernel_exit_restore(void);
/* Saved kernel context — written by user_mode_enter, read by kernel_exit_restore */
extern uint64_t saved_kernel_rsp;
extern uint64_t saved_kernel_rip;
extern uint64_t saved_kernel_rbp;
extern uint64_t saved_kernel_rbx;
extern uint64_t saved_kernel_r12;
extern uint64_t saved_kernel_r13;
extern uint64_t saved_kernel_r14;
extern uint64_t saved_kernel_r15;

#endif
