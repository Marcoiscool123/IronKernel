/* IronKernel v0.04 — test/link.ld  (user-mode ELF64 programs) */
OUTPUT_FORMAT("elf64-x86-64")
/* User programs are now 64-bit ELF to match the kernel's long mode. */

ENTRY(_start)

SECTIONS {
    . = 0x200000;
    /* Load at 2 MB — same as before, still within the 1 GB identity map
       set up by boot.asm. Virtual address == physical address. */

    .text   : { *(.text*)   }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*)   }
    .bss    : { *(.bss*) *(COMMON) }
}
