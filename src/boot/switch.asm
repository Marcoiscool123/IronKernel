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
                    ; (for first-time tasks: task_entry_sti trampoline below)

; ── task_entry_sti ────────────────────────────────────────────────────────
; First-time entry point for all tasks spawned via task_spawn.
; task_switch rets here on the first scheduling of a new task.
; rbx = function pointer (set up by task_spawn in the initial stack frame).
;
; Interrupt gate clears IF when the PIT fires to run sched_tick/task_switch.
; Without sti here, the new task runs with interrupts disabled forever:
; the PIT never fires, sched_tick is never called, and no other task gets CPU.
extern task_exit
global task_entry_sti
task_entry_sti:
    sti             ; re-enable interrupts before entering the task function
    call rbx        ; call the task function (pointer saved in rbx by task_spawn)
    jmp task_exit   ; if function returns without calling task_exit, do it now
