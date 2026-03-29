/* IronKernel v0.04 — gdt.c
   7-entry 64-bit GDT:
     [0] null  [1] kcode  [2] kdata  [3] ucode  [4] udata
     [5] TSS-low  [6] TSS-high  (TSS descriptor is 16 bytes = 2 slots)
*/
#include "gdt.h"
#include "tss.h"
#include "types.h"

static uint64_t    gdt[7];
static gdt_ptr64_t gdt_ptr;

/* encode_tss_desc — write a 64-bit TSS descriptor into gdt[5] and gdt[6].
   Intel SDM Vol.3A §7.2.3: the 64-bit TSS descriptor uses 16 bytes.
   Bytes 0-7 (low qword):
     [15:0]  limit[15:0]
     [39:16] base[23:0]
     [47:40] access: P=1 DPL=0 type=0x9 (64-bit TSS Available)
     [51:48] limit[19:16]
     [55:52] flags (G=0 for byte granularity)
     [63:56] base[31:24]
   Bytes 8-15 (high qword):
     [31:0]  base[63:32]
     [63:32] reserved (must be 0)
*/
static void encode_tss_desc(uint64_t base, uint32_t limit)
{
    uint64_t lo = 0;
    lo |= (uint64_t)(limit & 0xFFFF);             /* limit[15:0]  */
    lo |= (uint64_t)(base  & 0x00FFFFFF) << 16;   /* base[23:0]   */
    lo |= (uint64_t)0x89 << 40;                   /* P=1 DPL=0 type=TSS-avail */
    lo |= (uint64_t)((limit >> 16) & 0x0F) << 48; /* limit[19:16] */
    lo |= (uint64_t)((base >> 24) & 0xFF) << 56;  /* base[31:24]  */
    gdt[5] = lo;
    gdt[6] = (base >> 32) & 0xFFFFFFFF;           /* base[63:32]  */
}

void gdt_init(void)
{
    gdt[0] = 0;                     /* null — mandatory, selector 0 causes #GP */

    gdt[1] = 0x00AF9A000000FFFF;
    /* 64-bit kernel code DPL=0:
       access 0x9A = P|S|E|R   gran 0xAF = G=1 D/B=0 L=1 (64-bit code) */

    gdt[2] = 0x00CF92000000FFFF;
    /* kernel data DPL=0:
       access 0x92 = P|S|W     gran 0xCF = G=1 D/B=1 L=0 */

    gdt[3] = 0x00AFFA000000FFFF;
    /* 64-bit user code DPL=3:
       access 0xFA = P|DPL3|S|E|R   gran 0xAF = L=1 */

    gdt[4] = 0x00CFF2000000FFFF;
    /* user data DPL=3:
       access 0xF2 = P|DPL3|S|W */

    encode_tss_desc((uint64_t)tss_get(), (uint32_t)(sizeof_tss() - 1));
    /* TSS descriptor: base = address of our TSS, limit = sizeof(TSS)-1.
       On every ring-3 interrupt the CPU reads rsp0 from this TSS to find
       the kernel stack pointer. Without it, user-mode interrupts crash. */

    gdt_ptr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_ptr.base  = (uint64_t)gdt;

    gdt_flush(&gdt_ptr);    /* lgdt + retfq CS reload in gdt.asm */
    tss_flush(GDT_TSS_SEL); /* ltr — loads TSS into task register */
}
