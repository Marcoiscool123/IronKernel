/* IronKernel v0.04 — src/boot/linker.ld */
OUTPUT_FORMAT("elf64-x86-64")
/* Changed from elf32-i386. The kernel binary is now a 64-bit ELF.
   ld needs this to match the -f elf64 NASM output and -m elf_x86_64 flag. */

ENTRY(_start)

SECTIONS {
    . = 1M;
    /* Still load at 1 MB — the first megabyte remains reserved for BIOS,
       VGA buffer, and legacy hardware. This has not changed in long mode. */

    kernel_start = .;

    .multiboot2 : {
        KEEP(*(.multiboot2))
        /* GRUB scans the first 32 KB for the Multiboot2 magic.
           This section must appear first in the binary. */
    }

    .text ALIGN(4K) : {
        *(.text*)
    }

    .rodata ALIGN(4K) : {
        *(.rodata*)
    }

    .data ALIGN(4K) : {
        *(.data*)
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss*)
        . = ALIGN(4096);
        kernel_end = .;
        /* BSS includes boot_stack, pml4/pdpt/pd_table from boot.asm,
           saved_kernel_rsp from usermode.asm, and kernel data structures. */
    }
}
