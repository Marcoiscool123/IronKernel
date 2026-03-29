; IronKernel v0.04 — src/boot/irq.asm
; 64-bit IRQ stubs (IRQ 0-15, PIC-remapped to IDT vectors 32-47)
[BITS 64]

%macro IRQ_STUB 1
global irq%1
irq%1:
    push qword 0        ; fake error code (keeps frame uniform)
    push qword %1       ; IRQ number
    jmp  irq_common
%endmacro

IRQ_STUB 0    ; PIT timer
IRQ_STUB 1    ; PS/2 keyboard
IRQ_STUB 2    ; cascade (slave PIC)
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12   ; PS/2 mouse
IRQ_STUB 13
IRQ_STUB 14   ; ATA primary
IRQ_STUB 15   ; ATA secondary

; ── Common IRQ handler ────────────────────────────────────────────────────
; After 15 GPR pushes, IRQ number is at [rsp+120].
; System V AMD64 ABI: first integer arg goes in RDI.
; irq_handler(uint64_t irq_num) dispatches to the right driver callback.

extern irq_handler

irq_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov  edi, [rsp + 120]   ; irq_num (32-bit load, zero-extends to RDI)
    call irq_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16             ; discard irq_num + fake error code
    iretq
