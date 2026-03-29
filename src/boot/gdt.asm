; IronKernel v0.04 — src/boot/gdt.asm
; 64-bit GDT flush + TSS load
; Called from kernel/gdt.c after the full GDT is built.
[BITS 64]

; gdt_flush(gdt_ptr64_t *gdtr)
;   RDI = pointer to { uint16_t limit; uint64_t base } (10 bytes packed)
; We cannot do a far jump in C portably, so we use the retfq trick:
; push the target CS and RIP onto the stack, then retfq pops both,
; atomically reloading CS from our new GDT descriptor.
global gdt_flush
gdt_flush:
    lgdt [rdi]                  ; load new GDT — CS not yet reloaded

    lea  rax, [rel .reload_cs]  ; RIP-relative address of reload target
    push qword 0x08             ; new CS (kernel code selector)
    push rax                    ; new RIP
    retfq                       ; far return: pops RIP then CS, CS reloaded ✓

.reload_cs:
    mov ax, 0x10                ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; tss_flush(uint16_t sel)
;   DI = TSS selector (e.g. 0x28)
; LTR loads the selector into the hidden Task Register.
; The CPU reads the TSS on every ring-3→ring-0 transition to find rsp0.
; Without this call, interrupts from user mode use a garbage RSP and crash.
global tss_flush
tss_flush:
    ltr di
    ret
