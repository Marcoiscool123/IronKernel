/* IronKernel v0.04 — tss.c */
#include "tss.h"
#include "types.h"

static TSS64 tss;

void tss_init(uint64_t kernel_stack_top)
{
    /* zero entire struct — static is already zero, but be explicit */
    uint8_t *p = (uint8_t*)&tss;
    for (uint32_t i = 0; i < sizeof(TSS64); i++) p[i] = 0;

    tss.rsp0       = kernel_stack_top;
    /* rsp0: when an interrupt fires while in ring 3, the CPU loads this
       value into RSP before pushing the iretq frame. It must point to
       the TOP of a valid kernel stack. */

    tss.iomap_base = (uint16_t)sizeof(TSS64);
    /* Setting iomap_base = sizeof(TSS) means there is no I/O bitmap.
       All port I/O from ring 3 will cause a #GP. */
}

void tss_set_rsp0(uint64_t rsp0)
{
    tss.rsp0 = rsp0;
    /* Called by the scheduler when switching to a task that may enter
       user mode, so the correct kernel stack is always in the TSS. */
}

TSS64 *tss_get(void)  { return &tss; }
uint32_t sizeof_tss(void) { return (uint32_t)sizeof(TSS64); }
