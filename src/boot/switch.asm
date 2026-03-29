; IronKernel v0.04 — src/boot/switch.asm
; 64-bit preemptive context switch
;
; void task_switch(uint64_t *old_rsp, uint64_t new_rsp)
;   RDI = address of current task's rsp field (write saved RSP here)
;   RSI = saved RSP of the incoming task
;
; Callee-saved per System V AMD64 ABI: rbp rbx r12 r13 r14 r15
; (edi/esi/ebx/ebp from 32-bit are replaced by this set in 64-bit)
[BITS 64]
global task_switch

task_switch:
    push rbp        ; save caller-saved regs onto CURRENT task's kernel stack
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp  ; store current RSP into old task's task_t.rsp field

    mov rsp, rsi    ; switch to new task's kernel stack

    pop r15         ; restore incoming task's callee-saved registers
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret             ; jumps to wherever the new task was last interrupted
                    ; (for first-time tasks: the function pointer we put on
                    ;  its initial stack in task_spawn)
