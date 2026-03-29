/* IronKernel v0.04 — vmm.c
   boot.asm set up 4-level paging with 1 GB identity map (2MB huge pages).
   vmm_init() is a no-op — CR3 is already loaded and paging is active.
   vmm_map_page() handles addresses beyond the huge-paged region.
*/
#include "vmm.h"
#include "pmm.h"
#include "types.h"
#include "../drivers/vga.h"

/* Kernel page tables built by boot.asm */
extern uint64_t pd_table[];
extern uint64_t pdpt_table[];

/* Page table index extraction from a 64-bit virtual address */
#define PML4_IDX(v)  (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v)  (((v) >> 30) & 0x1FF)
#define PD_IDX(v)    (((v) >> 21) & 0x1FF)
#define PT_IDX(v)    (((v) >> 12) & 0x1FF)
#define PHYS_MASK    (~(uint64_t)0xFFF)   /* strip flag bits, keep address */

static inline uint64_t rd_cr3(void) {
    uint64_t v; __asm__ volatile("mov %%cr3,%0":"=r"(v)); return v;
}
static inline void wr_cr3(uint64_t v) {
    __asm__ volatile("mov %0,%%cr3"::"r"(v):"memory");
}
static inline void invlpg(uint64_t v) {
    __asm__ volatile("invlpg (%0)"::"r"(v):"memory");
}

static uint64_t g_kernel_cr3 = 0;

void vmm_init(void)
{
    /* Page tables were built by boot.asm — nothing to do here.
       CR3 is already pointing at pml4_table and paging is on. */
    g_kernel_cr3 = rd_cr3();
}

uint64_t vmm_kernel_cr3(void) { return g_kernel_cr3; }
void     vmm_load_cr3(uint64_t cr3) { wr_cr3(cr3); }

/* Create a per-process PML4.
   Kernel region (PD[0] = 0x000000–0x1FFFFF) is copied supervisor-only.
   User code/stack mappings are added later via vmm_map_page_in(). */
uint64_t vmm_create_user_pt(void)
{
    /* New PML4 */
    uint64_t pml4_phys = pmm_alloc_frame();
    if (!pml4_phys) return 0;
    uint64_t *pml4 = (uint64_t*)pml4_phys;
    for (int i = 0; i < 512; i++) pml4[i] = 0;

    /* New PDPT */
    uint64_t pdpt_phys = pmm_alloc_frame();
    if (!pdpt_phys) { pmm_free_frame(pml4_phys); return 0; }
    uint64_t *pdpt = (uint64_t*)pdpt_phys;
    for (int i = 0; i < 512; i++) pdpt[i] = 0;

    /* New PD — copy kernel's pd_table, strip USER bit on all entries */
    uint64_t pd_phys = pmm_alloc_frame();
    if (!pd_phys) { pmm_free_frame(pdpt_phys); pmm_free_frame(pml4_phys); return 0; }
    uint64_t *pd = (uint64_t*)pd_phys;
    for (int i = 0; i < 512; i++)
        pd[i] = pd_table[i] & ~PAGE_USER;   /* supervisor-only */

    /* Wire up: PML4[0] → new PDPT (USER so ring-3 can walk) */
    pml4[0] = pdpt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    /* PDPT[0] → new PD (first 1 GB: kernel + user code/stack) */
    pdpt[0] = pd_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    /* PDPT[3] → kernel's pd_table_high, supervisor-only.
       The VBE LFB lives at 0xE0000000 (covered by PDPT[3]).
       Without this entry, vga_print() page-faults when called from
       syscall/exception handlers running under the user CR3. */
    pdpt[3] = pdpt_table[3] & ~(uint64_t)PAGE_USER;

    return pml4_phys;
}

/* Map a 4KB page into an arbitrary page table (identified by cr3).
   If a huge page currently covers virt in that table, it is split
   into a 4KB PT first. Caller supplies physical frame via phys.
   Returns 0 on success, -1 on out-of-memory. */
int vmm_map_page_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pml4 = (uint64_t*)(cr3 & PHYS_MASK);

    if (!(pml4[PML4_IDX(virt)] & PAGE_PRESENT)) {
        uint64_t p = pmm_alloc_frame();
        if (!p) return -1;
        uint64_t *t = (uint64_t*)p;
        for (int i = 0; i < 512; i++) t[i] = 0;
        pml4[PML4_IDX(virt)] = p | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    uint64_t *pdpt = (uint64_t*)(pml4[PML4_IDX(virt)] & PHYS_MASK);

    if (!(pdpt[PDPT_IDX(virt)] & PAGE_PRESENT)) {
        uint64_t p = pmm_alloc_frame();
        if (!p) return -1;
        uint64_t *t = (uint64_t*)p;
        for (int i = 0; i < 512; i++) t[i] = 0;
        pdpt[PDPT_IDX(virt)] = p | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    if (pdpt[PDPT_IDX(virt)] & PAGE_HUGE) return 0;
    uint64_t *pd = (uint64_t*)(pdpt[PDPT_IDX(virt)] & PHYS_MASK);

    /* If a 2MB huge page covers this address, replace with a 4KB PT.
       Populate all 512 entries to match the original huge page so that
       existing kernel mappings in this 2MB region are not lost. */
    if (pd[PD_IDX(virt)] & PAGE_HUGE) {
        uint64_t huge_entry = pd[PD_IDX(virt)];
        uint64_t huge_base  = huge_entry & ~(uint64_t)0x1FFFFF;
        uint64_t huge_flags = huge_entry & 0xFFF & ~(uint64_t)PAGE_HUGE;
        uint64_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) return -1;
        uint64_t *pt = (uint64_t*)pt_phys;
        for (int i = 0; i < 512; i++)
            pt[i] = (huge_base + (uint64_t)i * PAGE_SIZE) | huge_flags;
        /* PD entry gets USER so ring-3 can walk through it */
        pd[PD_IDX(virt)] = pt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    } else if (!(pd[PD_IDX(virt)] & PAGE_PRESENT)) {
        uint64_t p = pmm_alloc_frame();
        if (!p) return -1;
        uint64_t *t = (uint64_t*)p;
        for (int i = 0; i < 512; i++) t[i] = 0;
        pd[PD_IDX(virt)] = p | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    uint64_t *pt = (uint64_t*)(pd[PD_IDX(virt)] & PHYS_MASK);

    pt[PT_IDX(virt)] = (phys & PHYS_MASK) | (flags & 0xFFF) | PAGE_PRESENT;
    return 0;
}

/* Deep-copy all user pages from parent_cr3 into a fresh page table.
   Used by SYS_FORK. Every user (PAGE_USER) leaf page gets a new physical
   frame with the same content. Kernel huge pages are shared read-only
   (same as vmm_create_user_pt — they are never freed by vmm_free_user_pt).
   Returns new CR3 on success, 0 on out-of-memory. */
uint64_t vmm_clone_user_pt(uint64_t parent_cr3)
{
    uint64_t child_cr3 = vmm_create_user_pt();
    if (!child_cr3) return 0;

    uint64_t *pml4 = (uint64_t*)(parent_cr3 & PHYS_MASK);
    if (!(pml4[0] & PAGE_PRESENT)) return child_cr3; /* no user mappings */

    uint64_t *pdpt = (uint64_t*)(pml4[0] & PHYS_MASK);
    if (!(pdpt[0] & PAGE_PRESENT)) return child_cr3;

    uint64_t *pd = (uint64_t*)(pdpt[0] & PHYS_MASK);
    for (int i = 0; i < 512; i++) {
        if (!(pd[i] & PAGE_PRESENT)) continue;
        if (pd[i] & PAGE_HUGE) continue;   /* kernel huge page — shared, skip */

        uint64_t *pt = (uint64_t*)(pd[i] & PHYS_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pt[j] & PAGE_PRESENT)) continue;
            if (!(pt[j] & PAGE_USER))    continue;

            /* Allocate a new frame and copy the page content. */
            uint64_t src_frame = pt[j] & PHYS_MASK;
            uint64_t dst_frame = pmm_alloc_frame();
            if (!dst_frame) { vmm_free_user_pt(child_cr3); return 0; }

            uint8_t *src = (uint8_t*)src_frame;
            uint8_t *dst = (uint8_t*)dst_frame;
            for (int k = 0; k < (int)PAGE_SIZE; k++) dst[k] = src[k];

            /* Virtual address: all user pages live under pml4[0]→pdpt[0],
               so PML4-idx=0, PDPT-idx=0, PD-idx=i, PT-idx=j. */
            uint64_t virt = ((uint64_t)i << 21) | ((uint64_t)j << 12);
            uint64_t flags = pt[j] & 0xFFF;   /* preserve R/W, U/S, etc. */
            if (vmm_map_page_in(child_cr3, virt, dst_frame, flags) != 0) {
                pmm_free_frame(dst_frame);
                vmm_free_user_pt(child_cr3);
                return 0;
            }
        }
    }
    return child_cr3;
}

/* Free all frames and page table structures belonging to a process.
   Huge page entries copied from the kernel PD are NOT freed — only
   PT frames we allocated (non-huge PD entries) and their data frames. */
void vmm_free_user_pt(uint64_t cr3)
{
    uint64_t *pml4 = (uint64_t*)(cr3 & PHYS_MASK);

    if (pml4[0] & PAGE_PRESENT) {
        uint64_t *pdpt = (uint64_t*)(pml4[0] & PHYS_MASK);
        if (pdpt[0] & PAGE_PRESENT) {
            uint64_t *pd = (uint64_t*)(pdpt[0] & PHYS_MASK);
            for (int i = 0; i < 512; i++) {
                if (!(pd[i] & PAGE_PRESENT)) continue;
                if (pd[i] & PAGE_HUGE) continue;   /* kernel huge page — skip */
                if (!(pd[i] & PAGE_USER)) continue; /* kernel-inherited PT — skip */
                /* PT we created */
                uint64_t *pt = (uint64_t*)(pd[i] & PHYS_MASK);
                for (int j = 0; j < 512; j++) {
                    if ((pt[j] & PAGE_PRESENT) && (pt[j] & PAGE_USER))
                        pmm_free_frame(pt[j] & PHYS_MASK);
                }
                pmm_free_frame(pd[i] & PHYS_MASK);  /* free PT frame */
            }
            pmm_free_frame(pdpt[0] & PHYS_MASK);    /* free PD frame */
        }
        pmm_free_frame(pml4[0] & PHYS_MASK);        /* free PDPT frame */
    }
    pmm_free_frame(cr3 & PHYS_MASK);                /* free PML4 frame */
}

/* Walk the 4-level table. Returns physical address or 0 if not mapped. */
uint64_t vmm_get_physical(uint64_t virt)
{
    uint64_t *pml4 = (uint64_t*)(rd_cr3() & PHYS_MASK);
    if (!(pml4[PML4_IDX(virt)] & PAGE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t*)(pml4[PML4_IDX(virt)] & PHYS_MASK);
    if (!(pdpt[PDPT_IDX(virt)] & PAGE_PRESENT)) return 0;
    if (  pdpt[PDPT_IDX(virt)] & PAGE_HUGE)  /* 1 GB page */
        return (pdpt[PDPT_IDX(virt)] & ~(uint64_t)0x3FFFFFFF) | (virt & 0x3FFFFFFF);
    uint64_t *pd = (uint64_t*)(pdpt[PDPT_IDX(virt)] & PHYS_MASK);
    if (!(pd[PD_IDX(virt)] & PAGE_PRESENT)) return 0;
    if (  pd[PD_IDX(virt)] & PAGE_HUGE)     /* 2 MB page — already mapped */
        return (pd[PD_IDX(virt)] & ~(uint64_t)0x1FFFFF) | (virt & 0x1FFFFF);
    uint64_t *pt = (uint64_t*)(pd[PD_IDX(virt)] & PHYS_MASK);
    if (!(pt[PT_IDX(virt)] & PAGE_PRESENT)) return 0;
    return (pt[PT_IDX(virt)] & PHYS_MASK) | (virt & 0xFFF);
}

/* Map a single 4KB page. For addresses already covered by the
   2MB identity map, this is a no-op — the huge page suffices.
   Returns 0 on success, -1 on out-of-memory. */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pml4 = (uint64_t*)(rd_cr3() & PHYS_MASK);

    /* PML4 */
    if (!(pml4[PML4_IDX(virt)] & PAGE_PRESENT)) {
        uint64_t p = pmm_alloc_frame();
        if (!p) return -1;
        uint64_t *t = (uint64_t*)p;
        for (int i=0;i<512;i++) t[i]=0;
        pml4[PML4_IDX(virt)] = p | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    uint64_t *pdpt = (uint64_t*)(pml4[PML4_IDX(virt)] & PHYS_MASK);

    /* PDPT */
    if (!(pdpt[PDPT_IDX(virt)] & PAGE_PRESENT)) {
        uint64_t p = pmm_alloc_frame();
        if (!p) return -1;
        uint64_t *t = (uint64_t*)p;
        for (int i=0;i<512;i++) t[i]=0;
        pdpt[PDPT_IDX(virt)] = p | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    if (pdpt[PDPT_IDX(virt)] & PAGE_HUGE) return 0; /* 1GB page covers this */
    uint64_t *pd = (uint64_t*)(pdpt[PDPT_IDX(virt)] & PHYS_MASK);

    /* PD */
    if (!(pd[PD_IDX(virt)] & PAGE_PRESENT)) {
        uint64_t p = pmm_alloc_frame();
        if (!p) return -1;
        uint64_t *t = (uint64_t*)p;
        for (int i=0;i<512;i++) t[i]=0;
        pd[PD_IDX(virt)] = p | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
        if (pd[PD_IDX(virt)] & PAGE_HUGE) {
        uint64_t huge_entry = pd[PD_IDX(virt)];
        uint64_t huge_base  = huge_entry & ~(uint64_t)0x1FFFFF;
        uint64_t huge_flags = (huge_entry & 0xFFF) & ~((uint64_t)0x80); // Remove HUGE bit
        uint64_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) return -1;
        uint64_t *pt = (uint64_t*)pt_phys;
        for (int i = 0; i < 512; i++)
            pt[i] = (huge_base + (uint64_t)i * 4096) | huge_flags;
        pd[PD_IDX(virt)] = pt_phys | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        wr_cr3(rd_cr3()); // Flush TLB
    }
    uint64_t *pt = (uint64_t*)(pd[PD_IDX(virt)] & PHYS_MASK);

    /* PT — install 4KB entry */
    pt[PT_IDX(virt)] = (phys & PHYS_MASK) | (flags & 0xFFF) | PAGE_PRESENT;
    invlpg(virt);
    return 0;
}

void vmm_page_fault(uint64_t err_code)
{
    uint64_t cr2;
    __asm__ volatile("mov %%cr2,%0":"=r"(cr2));
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_print("\n[PAGE FAULT] addr=0x");
    char buf[17]; buf[16]=0;
    uint64_t v=cr2;
    for(int i=15;i>=0;i--){buf[i]="0123456789ABCDEF"[v&0xF];v>>=4;}
    vga_print(buf);
    vga_print(" err=0x");
    v=err_code;
    for(int i=15;i>=0;i--){buf[i]="0123456789ABCDEF"[v&0xF];v>>=4;}
    vga_print(buf);
    for(;;) __asm__ volatile("cli;hlt");
}
