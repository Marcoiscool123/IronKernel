[BITS 64]
DEFAULT REL

extern vga_print
extern user_mode_active
global user_mode_enter
global saved_kernel_rsp
global saved_kernel_rip
global syscall_stub_64

section .data
saved_kernel_rsp: dq 0
saved_kernel_rip: dq 0
saved_kernel_rbp: dq 0
saved_kernel_rbx: dq 0
saved_kernel_r12: dq 0
saved_kernel_r13: dq 0
saved_kernel_r14: dq 0
saved_kernel_r15: dq 0

global saved_kernel_rbp
global saved_kernel_rbx
global saved_kernel_r12
global saved_kernel_r13
global saved_kernel_r14
global saved_kernel_r15

section .text

user_mode_enter:
    cli
    ; Save all callee-saved registers + return context
    mov  [rel saved_kernel_rbp], rbp
    mov  [rel saved_kernel_rbx], rbx
    mov  [rel saved_kernel_r12], r12
    mov  [rel saved_kernel_r13], r13
    mov  [rel saved_kernel_r14], r14
    mov  [rel saved_kernel_r15], r15
    mov  rax, [rsp]
    mov  [rel saved_kernel_rip], rax
    lea  rax, [rsp + 8]
    mov  [rel saved_kernel_rsp], rax
    mov  dword [rel user_mode_active], 1
    push 0x23
    push rsi
    push 0x202
    push 0x1B
    push rdi
    mov  ax, 0x23
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    iretq

; kernel_exit_restore — called by syscall_dispatch SYS_EXIT.
; Restores all callee-saved registers saved by user_mode_enter,
; clears user_mode_active, restores the kernel stack, and jumps
; back to the return address saved by user_mode_enter.
; This is a jump-back, not a normal return — it never comes back here.
global kernel_exit_restore
kernel_exit_restore:
    mov  rbp, [rel saved_kernel_rbp]
    mov  rbx, [rel saved_kernel_rbx]
    mov  r12, [rel saved_kernel_r12]
    mov  r13, [rel saved_kernel_r13]
    mov  r14, [rel saved_kernel_r14]
    mov  r15, [rel saved_kernel_r15]
    mov  dword [rel user_mode_active], 0
    mov  rsp, [rel saved_kernel_rsp]
    jmp  [rel saved_kernel_rip]

syscall_stub_64:
    push rax
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    pop  rax
    cmp  rax, 0
    je   .do_write
    cmp  rax, 1
    je   .do_exit
    iretq

.do_write:
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    mov  rdi, rbx
    call vga_print
    pop  r11
    pop  r10
    pop  r9
    pop  r8
    pop  rdi
    pop  rsi
    pop  rdx
    pop  rcx
    pop  rbx
    iretq

.do_exit:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  dword [rel user_mode_active], 0
    ; Restore ALL callee-saved registers before returning to kernel
    mov  rbp, [rel saved_kernel_rbp]
    mov  rbx, [rel saved_kernel_rbx]
    mov  r12, [rel saved_kernel_r12]
    mov  r13, [rel saved_kernel_r13]
    mov  r14, [rel saved_kernel_r14]
    mov  r15, [rel saved_kernel_r15]
    mov  rsp, [rel saved_kernel_rsp]
    jmp  [rel saved_kernel_rip]
