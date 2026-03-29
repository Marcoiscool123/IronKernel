/* IronKernel v0.04 — vmm.h  (4-level paging) */
#ifndef VMM_H
#define VMM_H
#include "types.h"

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_RW       (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_HUGE     (1ULL << 7)   /* PD entry: 2MB huge page */
#define PAGE_SIZE     4096ULL

void     vmm_init(void);
int      vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_get_physical(uint64_t virt);
void     vmm_page_fault(uint64_t err_code);

/* Per-process page table API */
uint64_t vmm_create_user_pt(void);
uint64_t vmm_clone_user_pt(uint64_t parent_cr3);   /* deep-copy for fork */
int      vmm_map_page_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_free_user_pt(uint64_t cr3);
void     vmm_load_cr3(uint64_t cr3);
uint64_t vmm_kernel_cr3(void);

#endif
