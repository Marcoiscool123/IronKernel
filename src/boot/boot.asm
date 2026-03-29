; IronKernel v0.04 — src/boot/boot.asm
; GRUB enters us in 32-bit protected mode even on a 64-bit machine.
; We build 4-level page tables, flip EFER.LME + CR0.PG, then
; far-jump into 64-bit long mode before calling kernel_main.
; nasm -f elf64

[BITS 32]

; ── Multiboot2 header ────────────────────────────────────────────────────
; GRUB scans the first 32 KB of the binary for this magic structure.
; Without it GRUB refuses to load the kernel entirely.

MB2_MAGIC equ 0xE85250D6
MB2_ARCH  equ 0
MB2_HLEN  equ (mb2_end - mb2_hdr)

section .multiboot2
align 8
mb2_hdr:
    dd MB2_MAGIC
    dd MB2_ARCH
    dd MB2_HLEN
    dd (0x100000000 - (MB2_MAGIC + MB2_ARCH + MB2_HLEN))
    ; checksum: all four dwords sum to zero mod 2^32 — GRUB verifies this

    align 8
    dw 0        ; end tag type
    dw 0        ; end tag flags
    dd 8        ; end tag size
mb2_end:

; ── BSS: page tables + boot info save + boot stack ──────────────────────
; Page tables must be 4096-byte aligned — the CPU checks this.
; Misaligned CR3 causes a silent triple fault before we can print anything.

section .bss
align 4096

global pml4_table
global pdpt_table
global pd_table
pml4_table:    resb 4096   ; 512 × 8-byte entries; [0] → pdpt_table
pdpt_table:    resb 4096   ; 512 × 8-byte entries; [0] → pd_table, [3] → pd_table_high
pd_table:      resb 4096   ; 512 × 2MB huge pages: 0x000000000 – 0x03FFFFFFF (1 GB)
pd_table_high: resb 4096   ; 512 × 2MB huge pages: 0x0C0000000 – 0x0FFFFFFFF (covers VBE fb)

align 8
mb_magic_sv: resd 1     ; save EAX (Multiboot2 magic = 0x36d76289)
mb_info_sv:  resd 1     ; save EBX (physical addr of Multiboot2 info)

align 16
boot_stack:     resb 32768
boot_stack_top:             ; stack grows down; ESP/RSP starts here

; ── Minimal 64-bit GDT for the transition ───────────────────────────────
; We need at least a null + 64-bit code descriptor to execute the far jump.
; gdt_init() in gdt.c installs the full 7-entry GDT (including TSS)
; immediately after kernel_main is entered.

section .rodata
align 8
gdt64:
    dq 0                    ; [0x00] null — mandatory
    dq 0x00AF9A000000FFFF   ; [0x08] 64-bit kernel code DPL=0  L=1
    dq 0x00CF92000000FFFF   ; [0x10] kernel data DPL=0
gdt64_end:

; 48-bit GDTR (used by lgdt in 32-bit mode — base is 32-bit)
gdt64_ptr32:
    dw gdt64_end - gdt64 - 1
    dd gdt64

; 80-bit GDTR (used by lgdt in 64-bit mode — base is 64-bit)
gdt64_ptr64:
    dw gdt64_end - gdt64 - 1
    dq gdt64

; ── 32-bit entry — GRUB lands here ──────────────────────────────────────

section .text
global _start
_start:
    cli                         ; no IDT yet — any interrupt = triple fault
    mov [mb_magic_sv], eax      ; save before CPUID stomps EAX
    mov [mb_info_sv],  ebx      ; save Multiboot2 info pointer
    mov esp, boot_stack_top

    ; ── CPUID: verify long mode support ─────────────────────────────────
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb  .no_lm
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)         ; EDX bit 29 = Long Mode (LM) bit
    jnz .lm_ok
.no_lm:
    cli
    hlt
    jmp .no_lm

.lm_ok:
    ; ── Build PML4 → PDPT → PD ──────────────────────────────────────────
    ; flags 0x03 = Present | Read/Write
    mov eax, pdpt_table
    or  eax, 0x07
    mov dword [pml4_table],     eax   ; PML4[0] → pdpt_table
    mov dword [pml4_table + 4], 0

    mov eax, pd_table
    or  eax, 0x07
    mov dword [pdpt_table],     eax   ; PDPT[0] → pd_table
    mov dword [pdpt_table + 4], 0

    ; Fill pd_table: 512 × 2MB huge pages covering 0x000000000 – 0x03FFFFFFF
    ; flags 0x87 = P | RW | User | PS
    mov edi, pd_table
    mov eax, 0x87
    mov ecx, 512
.fill_pd:
    mov dword [edi],     eax
    mov dword [edi + 4], 0
    add eax, 0x200000           ; next 2MB boundary
    add edi, 8
    loop .fill_pd

    ; PDPT[3] → pd_table_high (covers 0xC0000000 – 0xFFFFFFFF for VBE fb)
    mov eax, pd_table_high
    or  eax, 0x07
    mov dword [pdpt_table + 24],     eax  ; PDPT[3] low dword
    mov dword [pdpt_table + 28], 0        ; PDPT[3] high dword

    ; Fill pd_table_high: 512 × 2MB huge pages starting at phys 0xC0000000
    ; flags 0x83 = P | RW | PS
    mov edi, pd_table_high
    mov eax, 0xC0000083
    mov ecx, 512
.fill_pd_high:
    mov dword [edi],     eax
    mov dword [edi + 4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill_pd_high

    ; ── Load boot GDT, enable PAE, set CR3, set EFER.LME ───────────────
    lgdt [gdt64_ptr32]

    mov eax, cr4
    or  eax, (1 << 5)           ; CR4.PAE must be set before LME takes effect
    mov cr4, eax

    mov eax, pml4_table
    mov cr3, eax                ; CR3 = physical base of PML4

    mov ecx, 0xC0000080         ; EFER MSR address
    rdmsr
    or  eax, (1 << 8)           ; EFER.LME = Long Mode Enable
    wrmsr

    ; ── Enable paging → CPU enters long mode (compat sub-mode) ──────────
    mov eax, cr0
    or  eax, (1 << 31)          ; CR0.PG
    mov cr0, eax

    ; Far jump flushes the pipeline and reloads CS from our 64-bit
    ; code descriptor (L=1), switching the decoder to 64-bit mode.
    jmp dword 0x08:long_mode_entry

; ── 64-bit entry ─────────────────────────────────────────────────────────
[BITS 64]
long_mode_entry:
    mov ax, 0x10                ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload GDTR with 80-bit pointer (8-byte base) so the hidden
    ; GDT base register holds the full 64-bit address.
    lgdt [gdt64_ptr64]

    mov rsp, boot_stack_top
    xor rbp, rbp

    ; System V AMD64 ABI: first arg in RDI, second in RSI
    movzx rdi, dword [mb_magic_sv]
    movzx rsi, dword [mb_info_sv]

    extern kernel_main
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt
