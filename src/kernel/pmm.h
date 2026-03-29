#ifndef PMM_H
#define PMM_H

#include "types.h"
/* IRONKERNEL's own type definitions. No external headers. */

/* ── FRAME SIZE ─────────────────────────────────────────────────────
   A physical frame is the smallest unit of memory the PMM tracks.
   4096 bytes = 4KB = one page. This matches the x86 page size used
   by the MMU when paging is enabled. Frame size and page size must
   match or the paging Node will produce incorrect mappings.
   ─────────────────────────────────────────────────────────────── */

#define PMM_FRAME_SIZE  4096
/* 4096 bytes per frame. Power of 2 — all frame arithmetic uses
   bit shifts instead of division for speed. */

#define PMM_FRAMES_PER_BYTE  8
/* One bitmap byte tracks 8 frames — one bit per frame.
   Bit 0 of byte N = frame (N*8)+0. Bit 7 = frame (N*8)+7. */

/* ── MULTIBOOT2 STRUCTURES ──────────────────────────────────────────
   GRUB builds a Multiboot2 information structure in RAM and passes
   its address in ebx before jumping to _start. The structure is a
   sequence of variable-length tags. We define only the tags we need.
   ─────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t total_size;
    /* Total byte size of the entire Multiboot2 info structure.
       Used to know when we have walked past the last tag. */

    uint32_t reserved;
    /* Reserved field — always zero. Must not be read or written. */

} __attribute__((packed)) mb2_info_t;

typedef struct {
    uint32_t type;
    /* Tag type identifier. Type 6 = memory map tag.
       Type 0 = end tag (last tag in the structure).
       We skip all tags whose type is not 6. */

    uint32_t size;
    /* Total size of this tag in bytes including this header.
       Used to advance to the next tag: next = current + ALIGN(size, 8).
       Tags are always aligned to 8-byte boundaries. */

} __attribute__((packed)) mb2_tag_t;

typedef struct {
    uint32_t type;      /* Always 6 for memory map tag */
    uint32_t size;      /* Total size of this tag */
    uint32_t entry_size;
    /* Size of each memory map entry in bytes.
       Typically 24 bytes but may vary — we use this value
       rather than hardcoding 24 to remain spec-compliant. */

    uint32_t entry_version;
    /* Version of the entry format. Currently always 0.
       Future Multiboot2 revisions may increment this. */

} __attribute__((packed)) mb2_mmap_tag_t;

typedef struct {
    uint64_t base_addr;
    /* Physical start address of this memory region.
       64-bit to support machines with more than 4GB RAM.
       On our 32-bit kernel we only use the lower 32 bits
       for regions below 4GB. */

    uint64_t length;
    /* Byte length of this memory region.
       base_addr + length = first address PAST this region. */

    uint32_t type;
    /* Region type:
         1 = Available    ← safe to use for kernel/user memory
         2 = Reserved     ← do not touch (BIOS, hardware, ACPI)
         3 = ACPI Reclaimable ← can use after reading ACPI tables
         4 = NVS          ← ACPI Non-Volatile Storage, do not touch
         5 = Bad RAM      ← defective memory, never use
       We only allocate frames from type 1 regions. */

    uint32_t reserved;
    /* Padding — always zero. */

} __attribute__((packed)) mb2_mmap_entry_t;

#define MB2_MEMORY_AVAILABLE  1
/* The only type we will allocate from.
   All other types are treated as permanently reserved. */

/* ── PUBLIC INTERFACE ───────────────────────────────────────────── */

void     pmm_init(uint64_t mb2_addr);
/* Parse the Multiboot2 info structure at mb2_addr.
   Build the bitmap. Mark all reserved regions as used.
   Mark the kernel image itself as used.
   Must be called before pmm_alloc_frame or pmm_free_frame. */

uint64_t pmm_alloc_frame(void);
/* Scan the bitmap for the first free frame.
   Mark it as used. Return its physical address.
   Returns 0 if no free frames remain (out of memory). */

void     pmm_free_frame(uint64_t addr);
/* Mark the frame at physical address addr as free.
   addr must be 4KB-aligned and must have been allocated
   by pmm_alloc_frame. Freeing an unallocated frame corrupts
   the bitmap and causes future allocations to misbehave. */

uint64_t pmm_get_total_frames(void);
/* Returns total number of 4KB frames detected in the system.
   Derived from the highest usable address in the memory map. */

uint64_t pmm_get_free_frames(void);
/* Returns current number of free (unallocated) frames.
   Decremented by pmm_alloc_frame, incremented by pmm_free_frame. */

#endif
