#ifndef HEAP_H
#define HEAP_H

#include "types.h"

/* ── HEAP REGION ────────────────────────────────────────────────────
   The kernel heap lives at a fixed virtual address above the kernel
   image and the identity-mapped first 4MB. We choose 0x01000000
   (16MB) as the heap base — safely above the kernel, safely above
   any hardware memory-mapped regions in the first 1MB, and safely
   above the 4MB identity map. The heap grows upward from this base
   as more frames are mapped in on demand.
   ─────────────────────────────────────────────────────────────── */

#define HEAP_START      0x01000000
/* Virtual address where the heap begins: 16MB mark.
   All kmalloc pointers will be >= this value. */

#define HEAP_INITIAL_SIZE   0x100000
/* 1MB of heap mapped at init time — 256 physical frames.
   Sufficient for all kernel subsystems in current scope.
   Future Nodes may expand this dynamically via vmm_map_page. */

#define HEAP_MIN_SPLIT  16
/* Minimum number of bytes a remainder must be to justify splitting
   a free block into two. If the leftover would be smaller than this,
   we hand the entire block to the caller rather than creating a
   useless tiny fragment. Includes the header size. */

/* ── BLOCK HEADER ───────────────────────────────────────────────────
   Every allocation — free or used — is preceded by this header.
   The header is invisible to the caller: kmalloc returns a pointer
   to the byte immediately after the header.

   Layout in memory:

     [header: size + magic + flags]  ← heap_block_t
     [user data: 'size' bytes]       ← pointer returned to caller
     [next header ...]

   The free list is implicit — we walk forward through the heap
   by adding (sizeof header + block->size) to find the next block.
   ─────────────────────────────────────────────────────────────── */

#define HEAP_MAGIC  0xC0FFEE42
/* Magic number stored in every block header.
   On every access we verify the magic is intact.
   If it is not, the block header was overwritten — heap corruption.
   We halt immediately rather than propagating corrupt state. */

typedef struct {
    uint32_t magic;
    /* HEAP_MAGIC. Verified on every kmalloc and kfree.
       Corruption here means a buffer overflow destroyed the header. */

    uint32_t size;
    /* Size of the user data region in bytes — NOT including header.
       This is the number of bytes available to the caller. */

    uint8_t  is_free;
    /* 1 = this block is free and available for allocation.
       0 = this block is in use by a caller.
       We use uint8_t rather than a bitfield for simple, safe access. */

    uint8_t  _pad[3];
    /* Padding to align the struct to a 4-byte boundary.
       Without this, the compiler may insert implicit padding and
       the header size becomes unpredictable across compiler versions.
       Explicit padding makes the layout stable and auditable. */

} __attribute__((packed)) heap_block_t;

/* ── PUBLIC INTERFACE ───────────────────────────────────────────── */

void  heap_init(void);
/* Maps HEAP_INITIAL_SIZE bytes of physical frames into the heap
   virtual region. Initialises the first block header covering
   the entire mapped region as one large free block.
   Must be called after vmm_init. */

void* kmalloc(size_t size);
/* Allocate 'size' bytes from the kernel heap.
   Returns a pointer to the usable region (after the header).
   Returns 0 (NULL) if no block large enough is available.
   The returned pointer is always 4-byte aligned. */

void  kfree(void* ptr);
/* Return a previously kmalloc'd block to the heap.
   Marks it free and coalesces with any immediately following
   free block to prevent fragmentation.
   Passing a pointer not returned by kmalloc corrupts the heap. */

uint32_t heap_free_bytes(void);
/* Walk the entire heap and sum all free block sizes.
   Used for the boot-time diagnostic report. */

void heap_memstat(void);
/* Print a full heap statistics report to VGA:
   block counts, used/free bytes, largest free block,
   overhead, and live allocation count. */

void heap_dump_leaks(void);
/* Dump all currently live (unreleased) allocations tracked
   by the leak detector, with pointer, size, and caller address. */

#endif
