#include "heap.h"
#include "vmm.h"
#include "pmm.h"
#include "types.h"
#include "../drivers/vga.h"

/* ── LEAK TRACKER ────────────────────────────────────────────────── */

#define HEAP_LEAK_CAP 256

typedef struct {
    uintptr_t ptr;      /* pointer returned to caller  */
    uint32_t  size;     /* allocation size in bytes     */
    uintptr_t caller;   /* return address of kmalloc()  */
} heap_leak_t;

static heap_leak_t g_leaks[HEAP_LEAK_CAP];
static uint32_t    g_leak_count = 0;

static void leak_add(uintptr_t ptr, uint32_t size, uintptr_t caller)
{
    if (g_leak_count >= HEAP_LEAK_CAP) return;
    g_leaks[g_leak_count].ptr    = ptr;
    g_leaks[g_leak_count].size   = size;
    g_leaks[g_leak_count].caller = caller;
    g_leak_count++;
}

static void leak_del(uintptr_t ptr)
{
    for (uint32_t i = 0; i < g_leak_count; i++) {
        if (g_leaks[i].ptr == ptr) {
            /* Replace with last entry — O(1) removal */
            g_leaks[i] = g_leaks[--g_leak_count];
            return;
        }
    }
}

/* ── HEX PRINT HELPER (internal, no libc) ────────────────────────── */

static void heap_print_hex(uintptr_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    char buf[11]; buf[0]='0'; buf[1]='x'; buf[10]='\0';
    for (int i = 9; i >= 2; i--) { buf[i] = hx[v & 0xF]; v >>= 4; }
    vga_print(buf);
}

static void heap_print_uint(uint32_t n)
{
    if (n == 0) { vga_print("0"); return; }
    char buf[12]; buf[11] = '\0'; int i = 10;
    while (n > 0 && i >= 0) { buf[i--] = '0' + (n % 10); n /= 10; }
    vga_print(&buf[i + 1]);
}

/* ── HEAP STATE ─────────────────────────────────────────────────── */

static uintptr_t heap_end = HEAP_START;
/* Tracks the current top of the mapped heap region.
   heap_init advances this by HEAP_INITIAL_SIZE.
   Future expansion calls would advance it further. */

/* ── CORRUPTION GUARD ───────────────────────────────────────────── */

static void heap_panic(const char* msg)
{
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_print("\n[HEAP PANIC] ");
    vga_print(msg);
    vga_print("\n");
    for (;;) { __asm__ volatile ("hlt"); }
    /* Halt permanently on heap corruption.
       Continuing after a corrupt header would cause silent data
       destruction that manifests as a mysterious crash later.
       Stopping immediately preserves the evidence. */
}

/* ── HEAP INIT ──────────────────────────────────────────────────── */

void heap_init(void)
{
    /* Map HEAP_INITIAL_SIZE bytes of physical frames into the
       heap virtual region one page at a time. */
    uint32_t virt = HEAP_START;
    uint32_t mapped = 0;

    while (mapped < HEAP_INITIAL_SIZE) {
        uint32_t phys = pmm_alloc_frame();
        /* Request one free 4KB physical frame from the PMM. */

        if (!phys) {
            heap_panic("pmm_alloc_frame returned 0 during heap_init");
            return;
        }

        if (vmm_map_page(virt, phys, PAGE_PRESENT | PAGE_RW) != 0) {
            heap_panic("vmm_map_page failed during heap_init");
            return;
        }
        /* Map this physical frame into the heap virtual region.
           PAGE_RW — the heap must be writable by definition. */

        virt   += PAGE_SIZE;
        mapped += PAGE_SIZE;
    }

    heap_end = HEAP_START + HEAP_INITIAL_SIZE;
    /* Record the top of the mapped region. */

    /* Initialise one large free block covering the entire heap.
       The first block header sits at HEAP_START exactly.
       Its size covers everything after the header. */
    heap_block_t* first = (heap_block_t*)HEAP_START;
    first->magic   = HEAP_MAGIC;
    first->size    = HEAP_INITIAL_SIZE - sizeof(heap_block_t);
    /* Total mapped size minus the header itself = usable bytes. */
    first->is_free = 1;
    /* The entire heap starts as one large free block. */

    /* Install guard pages flanking the heap region.
       Mapping with flags=0 creates a NOT-PRESENT page table entry.
       Any access outside the valid heap window causes a page fault,
       catching both underflow (write before HEAP_START) and
       overflow (write past HEAP_START + HEAP_INITIAL_SIZE). */
    vmm_map_page(HEAP_START - PAGE_SIZE,           0, 0); /* underflow guard */
    vmm_map_page(HEAP_START + HEAP_INITIAL_SIZE,   0, 0); /* overflow  guard */

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("[HEAP] INITIALISED\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("       Base    : 0x01000000\n");
    vga_print("       Size    : 1 MB\n");
    vga_print("       Guards  : enabled\n");
}

/* ── KMALLOC ────────────────────────────────────────────────────── */

void* kmalloc(size_t size)
{
    if (size == 0) return 0;
    /* Zero-size allocations are undefined behaviour. Return null. */

    /* Align size to 4 bytes — every allocation is 4-byte aligned.
       This ensures all returned pointers are naturally aligned for
       any uint32_t or pointer access the caller might do. */
    size = (size + 3) & ~3;

    heap_block_t* block = (heap_block_t*)HEAP_START;
    /* Start walking from the first block at the heap base. */

    while ((uintptr_t)block < heap_end) {

        if (block->magic != HEAP_MAGIC) {
            heap_panic("corrupt magic in kmalloc walk");
            return 0;
        }

        if (block->is_free && block->size >= size) {
            /* Found a free block large enough. */

            if (block->size - size >= sizeof(heap_block_t) + HEAP_MIN_SPLIT) {
                /* Safe: block->size >= size is guaranteed by the outer check,
                   so the subtraction cannot underflow. Avoids potential
                   overflow in the original size + sizeof(...) + HEAP_MIN_SPLIT. */
                /* Block is large enough to split into two.
                   We carve off exactly what the caller needs and
                   leave the remainder as a new free block. */

                heap_block_t* remainder = (heap_block_t*)(
                    (uintptr_t)block + sizeof(heap_block_t) + size
                );
                /* New header starts immediately after the allocated region. */

                remainder->magic   = HEAP_MAGIC;
                remainder->size    = block->size - size - sizeof(heap_block_t);
                /* Remainder size = original size minus what we're taking
                   minus the header we are inserting for the remainder. */
                remainder->is_free = 1;
                /* Remainder is free — available for future allocations. */

                block->size = size;
                /* Shrink the current block to exactly what was requested. */
            }
            /* If block is too small to split, we hand the whole block
               to the caller. A few extra bytes are wasted — better than
               creating a fragment too small to ever satisfy a request. */

            block->is_free = 0;
            /* Mark as used before returning. */

            void *ret = (void*)((uintptr_t)block + sizeof(heap_block_t));
            leak_add((uintptr_t)ret, (uint32_t)block->size,
                     (uintptr_t)__builtin_return_address(0));
            return ret;
            /* Return pointer to the byte immediately after the header.
               The caller sees only their data region — never the header. */
        }

        /* Advance to the next block. */
        block = (heap_block_t*)((uintptr_t)block + sizeof(heap_block_t) + block->size);
    }

    /* No free block large enough was found. */
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_print("[HEAP] kmalloc: out of memory\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    return 0;
}

/* ── KFREE ──────────────────────────────────────────────────────── */

void kfree(void* ptr)
{
    if (!ptr) return;
    /* Freeing null is a no-op — matches standard C behaviour. */

    heap_block_t* block = (heap_block_t*)((uintptr_t)ptr - sizeof(heap_block_t));
    /* Step backward by header size to reach the block's header.
       The caller's pointer points to the data region — the header
       sits immediately before it. */

    if (block->magic != HEAP_MAGIC) {
        heap_panic("corrupt magic in kfree");
        return;
    }

    if (block->is_free) {
        heap_panic("double free detected");
        return;
        /* Freeing an already-free block is a serious bug.
           In an uninstrumented allocator this silently corrupts
           the free list. We catch it here explicitly. */
    }

    block->is_free = 1;
    leak_del((uintptr_t)ptr);
    /* Mark block as free and remove from leak tracker. */

    /* COALESCE — merge with the immediately following block if it
       is also free. This prevents the heap from fragmenting into
       many small blocks that individually cannot satisfy requests. */
    heap_block_t* next = (heap_block_t*)(
        (uintptr_t)block + sizeof(heap_block_t) + block->size
    );

    if ((uintptr_t)next < heap_end && next->magic == HEAP_MAGIC && next->is_free) {
        block->size += sizeof(heap_block_t) + next->size;
        /* Absorb next block's header and data into the current block.
           The next header disappears — it is now part of our data region.
           The result is one larger free block instead of two small ones. */
    }
}

/* ── HEAP FREE BYTES ────────────────────────────────────────────── */

uint32_t heap_free_bytes(void)
{
    uint32_t total = 0;
    heap_block_t* block = (heap_block_t*)HEAP_START;

    while ((uintptr_t)block < heap_end) {
        if (block->magic != HEAP_MAGIC) break;
        if (block->is_free) total += block->size;
        block = (heap_block_t*)((uintptr_t)block + sizeof(heap_block_t) + block->size);
    }
    return total;
}

/* ── HEAP_MEMSTAT ────────────────────────────────────────────────── */

void heap_memstat(void)
{
    uint32_t used_blk = 0, free_blk = 0;
    uint32_t used_bytes = 0, free_bytes = 0, largest_free = 0;
    uint32_t hdr_bytes = 0;

    heap_block_t* block = (heap_block_t*)HEAP_START;
    while ((uintptr_t)block < heap_end) {
        if (block->magic != HEAP_MAGIC) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_print("  [MEMSTAT] corrupt block detected during walk\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            return;
        }
        hdr_bytes += (uint32_t)sizeof(heap_block_t);
        if (block->is_free) {
            free_blk++;
            free_bytes += block->size;
            if (block->size > largest_free) largest_free = block->size;
        } else {
            used_blk++;
            used_bytes += block->size;
        }
        block = (heap_block_t*)((uintptr_t)block + sizeof(heap_block_t) + block->size);
    }
    uint32_t total_blk = used_blk + free_blk;

    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  Heap Statistics\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("    Region   : 0x01000000 \xe2\x80\x94 0x010FFFFF  (1 MB)\n");
    vga_print("    Blocks   : "); heap_print_uint(used_blk); vga_print(" used,  ");
    heap_print_uint(free_blk); vga_print(" free  (");
    heap_print_uint(total_blk); vga_print(" total)\n");
    vga_print("    Used     : "); heap_print_uint(used_bytes); vga_print(" bytes\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("    Free     : "); heap_print_uint(free_bytes); vga_print(" bytes");
    vga_print("  (largest: "); heap_print_uint(largest_free); vga_print(" bytes)\n");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_print("    Overhead : "); heap_print_uint(hdr_bytes); vga_print(" bytes  (");
    heap_print_uint(total_blk); vga_print(" headers x ");
    heap_print_uint((uint32_t)sizeof(heap_block_t)); vga_print(" bytes)\n");
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    vga_print("    Tracked  : "); heap_print_uint(g_leak_count); vga_print(" live allocations\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ── HEAP_DUMP_LEAKS ─────────────────────────────────────────────── */

void heap_dump_leaks(void)
{
    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  Live Allocations");
    vga_print(" (");
    heap_print_uint(g_leak_count);
    vga_print(")\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    if (g_leak_count == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_print("  (none — no leaks detected)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_print("  #    POINTER     SIZE       CALLER\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    for (uint32_t i = 0; i < g_leak_count; i++) {
        vga_print("  ");
        heap_print_uint(i);
        vga_print("  ");
        heap_print_hex(g_leaks[i].ptr);
        vga_print("  ");
        heap_print_uint(g_leaks[i].size);
        vga_print(" bytes  caller=");
        heap_print_hex(g_leaks[i].caller);
        vga_print("\n");
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}
