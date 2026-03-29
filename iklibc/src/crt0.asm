; IronKernel iklibc — crt0.asm
; C runtime startup: zeroes .bss, aligns stack, calls main(), calls exit().
; Linked first so _start is the ELF entry point.
[BITS 64]
DEFAULT REL

extern main
extern exit

; Linker-script symbols marking the .bss section boundaries.
; Their addresses ARE the boundaries — they have no storage.
extern _bss_start
extern _bss_end

global _start

section .text

_start:
    ; ── 1. Zero the .bss section ────────────────────────────────────
    ; GCC assumes static / global variables are zero-initialised.
    ; The ELF loader does NOT do this for us — we must do it here.
    lea  rdi, [_bss_start]      ; rdi = start of .bss
    lea  rcx, [_bss_end]        ; rcx = end of .bss (temporarily)
    sub  rcx, rdi               ; rcx = byte count of .bss
    xor  eax, eax               ; fill value = 0
    rep  stosb                  ; memset(.bss, 0, size)
    ; rep stosb: writes AL to [RDI], increments RDI, decrements RCX until 0.
    ; No SSE required — this is a plain x86 string instruction.

    ; ── 2. Align stack pointer to 16 bytes ──────────────────────────
    ; x86-64 System V ABI: RSP must be 16-byte aligned before any CALL.
    ; The ELF loader sets RSP to USER_STACK_TOP which is already aligned,
    ; but we AND just in case.
    and  rsp, ~0xF

    ; ── 3. Call main(void) ──────────────────────────────────────────
    ; IronKernel has no command-line arguments: argc=0, argv=NULL.
    xor  edi, edi               ; first arg  (argc) = 0
    xor  esi, esi               ; second arg (argv) = NULL
    call main

    ; ── 4. Pass main()'s return value to exit() ─────────────────────
    ; exit() calls SYS_EXIT and never returns.
    mov  edi, eax               ; exit status = main() return value
    call exit

    ; ── 5. Backstop — should never reach here ───────────────────────
    mov  eax, 1                 ; SYS_EXIT
    int  0x80
.hang:
    hlt
    jmp  .hang
