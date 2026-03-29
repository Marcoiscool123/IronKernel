#include "pmm.h"
#include "types.h"
#include "../drivers/vga.h"
/* pmm.h : our own PMM types and declarations
   types.h : uint8_t, uint32_t, uint64_t, size_t
   vga.h   : vga_print, vga_set_color — for boot-time memory report */

/* ── BITMAP STORAGE ─────────────────────────────────────────────────
   The bitmap lives in the kernel's .bss section — statically
   allocated at compile time. 32768 bytes = 262144 bits = 262144
   frames × 4096 bytes per frame = 1GB of addressable physical RAM.
   For machines with more than 1GB we extend coverage by treating
   higher frames as permanently reserved. A future Node will expand
   this to a dynamic bitmap allocated from the frames themselves.
   ─────────────────────────────────────────────────────────────── */

#define PMM_MAX_FRAMES  262144
/* Maximum frames the bitmap can track.
   262144 frames × 4KB = 1GB. Sufficient for QEMU's 256MB default
   and most development machines up to 1GB. */

#define PMM_BITMAP_SIZE  (PMM_MAX_FRAMES / PMM_FRAMES_PER_BYTE)
/* 262144 / 8 = 32768 bytes = 32KB of bitmap storage.
   Small enough to live in .bss without bloating the kernel. */

static uint8_t pmm_bitmap[PMM_BITMAP_SIZE];
/* The bitmap itself. All zeros at load time (BSS is zeroed).
   Zero bit = free frame. One bit = used frame.
   We start with everything marked free, then mark reserved
   regions used during pmm_init. */

static uint64_t pmm_total_frames = 0;
/* Total number of frames detected from the memory map.
   Set once during pmm_init. Never changes after that. */

static uint64_t pmm_free_frame_count = 0;
/* Running count of free frames. Decremented on alloc, incremented
   on free. Used by pmm_get_free_frames() without scanning bitmap. */

/* ── LINKER SYMBOLS ─────────────────────────────────────────────────
   These symbols are defined by the linker script (linker.ld).
   They mark the physical start and end of our kernel in memory.
   We mark all frames occupied by the kernel as used during init
   so the allocator never hands out memory the kernel is sitting in.
   ─────────────────────────────────────────────────────────────── */

extern uintptr_t kernel_start;
extern uintptr_t kernel_end;
/* Declared as uint32_t but used as addresses — we take &kernel_start
   to get the actual numeric address. The linker places these symbols
   at the precise byte boundaries of the kernel image. */

/* ── BITMAP OPERATIONS ──────────────────────────────────────────── */

static void pmm_set_frame(uint32_t frame_addr)
{
    uint64_t frame  = frame_addr / PMM_FRAME_SIZE;
    /* Convert byte address to frame index.
       Frame 0 = addresses 0x0000–0x0FFF
       Frame 1 = addresses 0x1000–0x1FFF  etc. */

    uint64_t byte   = frame / PMM_FRAMES_PER_BYTE;
    /* Which byte in the bitmap holds this frame's bit. */

    uint8_t  bit    = frame % PMM_FRAMES_PER_BYTE;
    /* Which bit within that byte represents this frame. */

    pmm_bitmap[byte] |= (1 << bit);
    /* Set the bit to 1 — mark frame as used.
       OR with a mask that has only the target bit set.
       All other bits in the byte are unaffected. */
}

static void pmm_clear_frame(uint32_t frame_addr)
{
    uint64_t frame  = frame_addr / PMM_FRAME_SIZE;
    uint64_t byte   = frame / PMM_FRAMES_PER_BYTE;
    uint8_t  bit    = frame % PMM_FRAMES_PER_BYTE;

    pmm_bitmap[byte] &= ~(1 << bit);
    /* Clear the bit to 0 — mark frame as free.
       AND with the inverse of the target bit mask.
       All other bits in the byte are unaffected. */
}

static uint32_t pmm_test_frame(uint32_t frame_addr)
{
    uint64_t frame  = frame_addr / PMM_FRAME_SIZE;
    uint64_t byte   = frame / PMM_FRAMES_PER_BYTE;
    uint8_t  bit    = frame % PMM_FRAMES_PER_BYTE;

    return pmm_bitmap[byte] & (1 << bit);
    /* Returns non-zero if frame is used, zero if free.
       Used by pmm_alloc_frame to skip already-used frames. */
}

/* ── SIMPLE INTEGER PRINTER ─────────────────────────────────────── */

static void print_uint32(uint32_t n)
{
    /* We have no printf. We have no sprintf. We build our own.
       Converts a uint32_t to decimal digits and prints via vga_print.
       Handles n=0 as a special case since the loop body never fires. */
    if (n == 0) { vga_print("0"); return; }

    char buf[12];
    /* Maximum uint32_t = 4294967295 = 10 digits + null terminator. */

    int i = 10;
    buf[11] = '\0';
    /* Null-terminate the buffer. We fill it right-to-left. */

    while (n > 0 && i >= 0) {
        buf[i--] = '0' + (n % 10);
        /* Extract the rightmost decimal digit.
           n % 10 gives 0–9. Adding '0' converts to ASCII '0'–'9'. */
        n /= 10;
        /* Shift right by one decimal place. */
    }
    vga_print(&buf[i + 1]);
    /* Print from the first digit we wrote (not from buf[0]). */
}

/* ── PUBLIC FUNCTIONS ───────────────────────────────────────────── */

void pmm_init(uint64_t mb2_addr)
{
    /* Start with the full bitmap marked as USED (all ones).
       We then selectively mark regions as FREE only if the
       memory map explicitly says they are available.
       This is safer than the inverse — unknown regions stay reserved. */
    for (size_t i = 0; i < PMM_BITMAP_SIZE; i++) {
        pmm_bitmap[i] = 0xFF;
        /* 0xFF = 11111111 = all eight frames in this byte are used. */
    }

    /* Walk the Multiboot2 tag list to find the memory map tag. */
    mb2_info_t* mb2 = (mb2_info_t*)mb2_addr;
    /* Cast the raw address to our mb2_info_t pointer.
       The first 8 bytes are total_size and reserved. Tags follow. */

    mb2_tag_t* tag = (mb2_tag_t*)(mb2_addr + 8);
    /* First tag starts immediately after the 8-byte header.
       We walk forward through tags until we find type 6 (mmap)
       or type 0 (end of tags). */

    mb2_mmap_tag_t* mmap_tag = 0;
    /* Will point to the memory map tag once we find it. */

    while (tag->type != 0) {
        /* type 0 = end tag. Stop when we hit it. */

        if (tag->type == 6) {
            mmap_tag = (mb2_mmap_tag_t*)tag;
            /* Found the memory map tag. Save it and break. */
            break;
        }

        uintptr_t next = (uintptr_t)tag + ((tag->size + 7) & ~7);
        /* Advance to next tag. Tags are padded to 8-byte alignment.
           (size + 7) & ~7 rounds size up to next multiple of 8.
           This handles any tag whose size is not already aligned. */
        tag = (mb2_tag_t*)next;
    }

    if (!mmap_tag) {
        /* No memory map found — GRUB did not provide one.
           This should never happen with a Multiboot2-compliant loader
           but we handle it defensively rather than crashing silently. */
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("[PMM] FATAL: No Multiboot2 memory map found.\n");
        return;
    }

    /* Walk each entry in the memory map. */
    uintptr_t entry_addr = (uintptr_t)mmap_tag + sizeof(mb2_mmap_tag_t);
    /* First entry starts immediately after the tag header. */

    uintptr_t mmap_end   = (uintptr_t)mmap_tag + mmap_tag->size;
    /* Address of the first byte past the last entry. */

    uint64_t highest_addr = 0;
    /* Track the highest usable address to compute total frame count. */

    while (entry_addr < mmap_end) {
        mb2_mmap_entry_t* entry = (mb2_mmap_entry_t*)entry_addr;

        if (entry->type == MB2_MEMORY_AVAILABLE) {
            /* This region is usable RAM. Mark its frames as free. */

            uint64_t base = entry->base_addr;
            uint64_t len  = entry->length;
            /* We truncate to 32 bits — safe for <4GB machines.
               A future Node will handle 64-bit PAE addressing. */

            uint64_t end  = base + len;

            if ((uint64_t)(entry->base_addr + entry->length) > highest_addr) {
                highest_addr = entry->base_addr + entry->length;
                /* Track highest usable address for total frame count. */
            }

            /* Mark each frame in this region as free. */
            uint32_t frame = base;
            while (frame < end && frame < (PMM_MAX_FRAMES * PMM_FRAME_SIZE)) {
                pmm_clear_frame(frame);
                /* Clear bit = mark as free. */
                pmm_free_frame_count++;
                frame += PMM_FRAME_SIZE;
            }
        }

        entry_addr += mmap_tag->entry_size;
        /* Advance by entry_size — NOT sizeof(mb2_mmap_entry_t).
           The spec may define larger entries in future versions.
           Using entry_size keeps us compliant with any Multiboot2 revision. */
    }

    /* Compute total frame count from highest usable address. */
    pmm_total_frames = (uint32_t)(highest_addr / PMM_FRAME_SIZE);
    if (pmm_total_frames > PMM_MAX_FRAMES) {
        pmm_total_frames = PMM_MAX_FRAMES;
        /* Cap at our bitmap's maximum coverage. */
    }

    /* Mark the first 1MB as reserved — BIOS, VGA buffer, IVT.
       Even if the memory map says some of this is available,
       the hardware uses it and we must not overwrite it. */
    uint64_t addr = 0;
    while (addr < 0x100000) {
        /* 0x100000 = 1MB. Everything below belongs to legacy hardware. */
        if (!pmm_test_frame(addr)) {
            pmm_set_frame(addr);
            if (pmm_free_frame_count > 0) pmm_free_frame_count--;
        }
        addr += PMM_FRAME_SIZE;
    }

    /* Mark all frames occupied by the kernel itself as reserved.
       kernel_start and kernel_end come from our linker script.
       If we allocated a frame the kernel is sitting in, we would
       overwrite our own code — an instant and silent crash. */
    uint64_t k_start = (uintptr_t)&kernel_start & ~(PMM_FRAME_SIZE - 1);
    uint64_t k_end   = ((uintptr_t)&kernel_end + PMM_FRAME_SIZE - 1)
                        & ~(PMM_FRAME_SIZE - 1);
    /* Align start down and end up to frame boundaries.
       This ensures partial frames at kernel edges are fully reserved. */

    addr = k_start;
    while (addr < k_end) {
        if (!pmm_test_frame(addr)) {
            pmm_set_frame(addr);
            if (pmm_free_frame_count > 0) pmm_free_frame_count--;
        }
        addr += PMM_FRAME_SIZE;
    }

    /* Also reserve the bitmap itself — it lives in .bss inside the kernel,
       so the kernel reservation above covers it automatically.
       No additional action needed. */

    /* Print memory report to screen. */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("[PMM] MEMORY MAP PARSED\n");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("      Total frames : ");
    print_uint32(pmm_total_frames);
    vga_print("\n");

    vga_print("      Free  frames : ");
    print_uint32(pmm_free_frame_count);
    vga_print("\n");

    vga_print("      Free  memory : ");
    print_uint32(pmm_free_frame_count * 4);
    vga_print(" KB\n");
}

uint64_t pmm_alloc_frame(void)
{
    if (pmm_free_frame_count == 0) {
        return 0;
        /* Out of memory. Caller must handle this.
           Returning 0 is safe — physical address 0 is in the
           reserved first-1MB region and will never be legitimately
           allocated. A null check catches this condition. */
    }

    for (uint32_t i = 0; i < PMM_BITMAP_SIZE; i++) {
        if (pmm_bitmap[i] == 0xFF) continue;
        /* All 8 frames in this byte are used — skip entire byte.
           This is the key performance win of the bitmap: we skip
           8 frames per iteration instead of checking one at a time. */

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (!(pmm_bitmap[i] & (1 << bit))) {
                /* Found a free bit. Calculate the frame address. */
                uint64_t frame_addr = (i * PMM_FRAMES_PER_BYTE + bit)
                                      * PMM_FRAME_SIZE;
                pmm_set_frame(frame_addr);
                /* Mark as used before returning. */
                pmm_free_frame_count--;
                return frame_addr;
                /* Return the physical address of this frame.
                   Caller owns this 4KB block until they call pmm_free_frame. */
            }
        }
    }

    return 0;
    /* Should never reach here if free_frame_count was accurate.
       Defensive return in case of bitmap/counter desynchronization. */
}

void pmm_free_frame(uint64_t addr)
{
    pmm_clear_frame(addr);
    /* Mark the bit as zero — frame is free again. */
    pmm_free_frame_count++;
    /* Increment free count — available for next alloc. */
}

uint64_t pmm_get_total_frames(void) { return pmm_total_frames; }
uint64_t pmm_get_free_frames(void)  { return pmm_free_frame_count; }
