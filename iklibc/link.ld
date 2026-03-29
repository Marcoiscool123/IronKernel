/* IronKernel iklibc — link.ld
   Linker script for user programs built with iklibc.
   Adds _bss_start / _bss_end symbols so crt0.asm can zero .bss. */
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    . = 0x200000;

    .text   : { *(.text*)   }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*)   }

    _bss_start = .;
    .bss    : { *(.bss*) *(COMMON) }
    _bss_end = .;
}
