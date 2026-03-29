; IronKernel v0.04 — src/boot/idt.asm
; 64-bit ISR stubs (vectors 0-31) + syscall stub (0x80)
; Each stub creates a uniform stack frame so C receives interrupt_frame_t*
[BITS 64]

; ISR_NOERR: CPU does NOT push an error code — we push 0 to keep layout uniform
; ISR_ERR  : CPU already pushed an error code before our stub runs
; In both cases stack on entry to isr_common is:
;   [rsp+0]  int_num   [rsp+8]  err_code   [rsp+16] rip  ...

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0        ; fake error code — CPU did not push one
    push qword %1       ; interrupt vector number
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    ; CPU already pushed error code at [rsp]; our push goes on top
    push qword %1       ; interrupt vector number
    jmp  isr_common
%endmacro

ISR_NOERR 0    ; #DE  Divide-by-Zero
ISR_NOERR 1    ; #DB  Debug
ISR_NOERR 2    ;      NMI
ISR_NOERR 3    ; #BP  Breakpoint
ISR_NOERR 4    ; #OF  Overflow
ISR_NOERR 5    ; #BR  Bound Range
ISR_NOERR 6    ; #UD  Invalid Opcode
ISR_NOERR 7    ; #NM  Device Not Available
ISR_ERR   8    ; #DF  Double Fault         (error code always 0)
ISR_NOERR 9    ;      Coprocessor Overrun  (legacy, never fires on modern hw)
ISR_ERR   10   ; #TS  Invalid TSS
ISR_ERR   11   ; #NP  Segment Not Present
ISR_ERR   12   ; #SS  Stack-Segment Fault
ISR_ERR   13   ; #GP  General Protection Fault
ISR_ERR   14   ; #PF  Page Fault
ISR_NOERR 15   ;      Reserved
ISR_NOERR 16   ; #MF  x87 FP Exception
ISR_ERR   17   ; #AC  Alignment Check
ISR_NOERR 18   ; #MC  Machine Check
ISR_NOERR 19   ; #XF  SIMD FP Exception
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30   ; #SX  Security Exception
ISR_NOERR 31

; Syscall gate — int 0x80 from ring 3 (DPL=3 in IDT entry)
global syscall_stub
syscall_stub:
    push qword 0        ; fake error code
    push qword 0x80     ; vector 0x80
    jmp  isr_common

; ── Common handler ────────────────────────────────────────────────────────
; Stack on arrival (int_num + err_code already pushed by stub):
;   [rsp+0]  int_num   [rsp+8]  err_code   [rsp+16] rip   [rsp+24] cs
;   [rsp+32] rflags    [rsp+40] rsp(user)  [rsp+48] ss(user)
;
; After pushing 15 GPRs (120 bytes), RSP points at start of interrupt_frame_t.
; int_num is then at [rsp+120], err_code at [rsp+128], rip at [rsp+136].

extern isr_handler

isr_common:
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

    mov  rdi, rsp           ; arg1 = pointer to interrupt_frame_t on stack
    call isr_handler        ; C handler — may not return for SYS_EXIT

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

    add rsp, 16             ; discard int_num + err_code pushed by stubs
    iretq                   ; 64-bit iret: pops RIP, CS, RFLAGS, RSP, SS
