/* IronKernel — sched.c */
#include "sched.h"
#include "heap.h"
#include "tss.h"
#include "vmm.h"
#include "pipe.h"
#include "../drivers/vga.h"

static task_t tasks[MAX_TASKS];
static int    task_count   = 0;
static int    current_task = 0;
static int    sched_ready  = 0;
volatile int  user_mode_active = 0;

void sched_init(void)
{
    tasks[0].id         = 0;
    tasks[0].state      = TASK_RUNNING;
    tasks[0].stack      = 0;
    tasks[0].ticks      = 0;
    tasks[0].rsp        = 0;            /* filled on first task_switch away */
    tasks[0].cr3          = vmm_kernel_cr3();
    tasks[0].kstack_top   = tss_get()->rsp0;  /* boot-time ring-0 stack */
    tasks[0].irq_stack    = 0;                /* task 0 stays in ring 0 */
    tasks[0].irq_stack_top = tasks[0].kstack_top;
    const char *n = "shell";
    int i = 0;
    while (n[i] && i < 15) { tasks[0].name[i] = n[i]; i++; }
    tasks[0].name[i] = 0;

    /* Shell starts with stdin=keyboard, stdout/stderr=VGA. */
    tasks[0].fds[0].type = FD_STDIN;
    tasks[0].fds[1].type = FD_STDOUT;
    tasks[0].fds[2].type = FD_STDOUT;
    for (int j = 3; j < TASK_MAX_FDS; j++) tasks[0].fds[j].type = FD_NONE;

    tasks[0].win_id     = -1;
    tasks[0].elf_name[0] = 0;
    tasks[0].elf_ext[0]  = 0;
    task_count  = 1;
    sched_ready = 1;
}

int task_spawn(const char *name, void (*func)(void))
{
    if (task_count >= MAX_TASKS) return -1;
    __asm__ volatile("cli");

    int id   = task_count;
    task_t *t = &tasks[id];
    t->id         = (uint32_t)id;
    t->state      = TASK_READY;
    t->ticks      = 0;
    t->cr3        = vmm_kernel_cr3();
    t->stack      = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!t->stack) { __asm__ volatile("sti"); return -1; }
    t->kstack_top = (uint64_t)(t->stack + TASK_STACK_SIZE);
    t->irq_stack  = (uint8_t*)kmalloc(TASK_IRQ_STACK_SIZE);
    if (!t->irq_stack) { kfree(t->stack); __asm__ volatile("sti"); return -1; }
    t->irq_stack_top = (uint64_t)(t->irq_stack + TASK_IRQ_STACK_SIZE);

    int i = 0;
    while (name[i] && i < 15) { t->name[i] = name[i]; i++; }
    t->name[i] = 0;
    t->win_id     = -1;
    t->elf_name[0] = 0;
    t->elf_ext[0]  = 0;

    /* Build initial stack frame to match what task_switch pops:
       task_switch does: pop r15 r14 r13 r12 rbx rbp  then  ret
       So we push (from top of stack downward):
         task_exit   (return addr if func() returns)
         func        (the ret in task_switch jumps here first time)
         rbp=0 rbx=0 r12=0 r13=0 r14=0 r15=0  (lowest address / top of stack) */
    uint64_t *sp = (uint64_t*)(t->stack + TASK_STACK_SIZE);
    *(--sp) = (uint64_t)task_exit;  /* guard: if func returns */
    *(--sp) = (uint64_t)func;       /* first-time ret target  */
    *(--sp) = 0;                    /* rbp */
    *(--sp) = 0;                    /* rbx */
    *(--sp) = 0;                    /* r12 */
    *(--sp) = 0;                    /* r13 */
    *(--sp) = 0;                    /* r14 */
    *(--sp) = 0;                    /* r15 — top of frame, RSP points here */
    t->rsp = (uint64_t)sp;

    task_count++;
    __asm__ volatile("sti");
    return id;
}

void task_exit(void)
{
    __asm__ volatile("cli");
    tasks[current_task].state = TASK_DEAD;
    __asm__ volatile("sti");
    sched_yield();
    while(1) __asm__ volatile("hlt");
}

/* ── fork_child_trampoline ──────────────────────────────────────────────
   First function executed by a fork child when the scheduler picks it up.
   task_switch rets here (child's initial kernel stack has this as the
   return address). We load the child's CR3, set TSS.rsp0, then set RSP
   to the saved InterruptFrame and iretq directly to user mode with RAX=0.
   This function never returns. */
static __attribute__((noinline)) void fork_child_trampoline(void)
{
    int id    = sched_current_id();
    task_t *me = &tasks[id];

    vmm_load_cr3(me->cr3);
    tss_set_rsp0(me->irq_stack_top);

    /* Point RSP at the fork_frame's r15 field (start of struct) and
       pop all 15 GPRs in the same order isr_common pushed them, then
       skip int_num+err_code (16 bytes), set user data segments, iretq. */
    __asm__ volatile(
        "mov %0, %%rsp\n\t"
        "pop %%r15\n\t"
        "pop %%r14\n\t"
        "pop %%r13\n\t"
        "pop %%r12\n\t"
        "pop %%r11\n\t"
        "pop %%r10\n\t"
        "pop %%r9\n\t"
        "pop %%r8\n\t"
        "pop %%rbp\n\t"
        "pop %%rdi\n\t"
        "pop %%rsi\n\t"
        "pop %%rdx\n\t"
        "pop %%rcx\n\t"
        "pop %%rbx\n\t"
        /* Set user data segments now (rax is free, popped last). */
        "mov $0x23, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pop %%rax\n\t"      /* rax = fork_frame.rax = 0  (child return) */
        "add $16, %%rsp\n\t" /* skip int_num + err_code                  */
        "iretq"              /* pops rip, cs, rflags, rsp(user), ss      */
        :: "r"((uint64_t)&me->fork_frame)
        : "memory"
    );
    __builtin_unreachable();
}

/* ── sched_fork ─────────────────────────────────────────────────────────
   Called from SYS_FORK handler with the parent's interrupt frame.
   Clones the parent's user address space and creates a new runnable task
   that will resume from the same point with RAX=0.
   Returns child task id on success, -1 on failure. */
int sched_fork(InterruptFrame *frame)
{
    __asm__ volatile("cli");

    /* Scan for a recycled TASK_DEAD slot first; fall back to appending. */
    int id = -1;
    for (int i = 1; i < task_count; i++) {
        if (tasks[i].state == TASK_DEAD && !tasks[i].is_forked) {
            id = i; break;
        }
    }
    if (id < 0) {
        if (task_count >= MAX_TASKS) { __asm__ volatile("sti"); return -1; }
        id = task_count;
    }

    int parent_id = current_task;
    task_t *parent = &tasks[parent_id];

    /* Deep-copy user address space.
       Must run under kernel CR3 — the user's page table has split some
       huge pages for user code, leaving holes in the identity map.
       pmm_alloc_frame() may return frames in those holes, causing a
       fault when vmm_create_user_pt writes to them as virtual addresses. */
    uint64_t parent_cr3 = parent->cr3;
    vmm_load_cr3(vmm_kernel_cr3());
    uint64_t child_cr3 = vmm_clone_user_pt(parent_cr3);
    vmm_load_cr3(parent_cr3);
    if (!child_cr3) { __asm__ volatile("sti"); return -1; }

    task_t *child = &tasks[id];

    child->id        = (uint32_t)id;
    child->state     = TASK_READY;
    child->ticks     = 0;
    child->cr3       = child_cr3;
    child->parent_id = parent_id;
    child->is_forked = 1;
    child->exit_code = 0;

    const char *n = "child";
    int i = 0;
    while (n[i] && i < 15) { child->name[i] = n[i]; i++; }
    child->name[i] = 0;

    child->stack = (uint8_t*)kmalloc(TASK_STACK_SIZE);
    if (!child->stack) {
        vmm_free_user_pt(child_cr3);
        __asm__ volatile("sti");
        return -1;
    }
    child->kstack_top = (uint64_t)(child->stack + TASK_STACK_SIZE);
    child->irq_stack  = (uint8_t*)kmalloc(TASK_IRQ_STACK_SIZE);
    if (!child->irq_stack) {
        kfree(child->stack);
        vmm_free_user_pt(child_cr3);
        __asm__ volatile("sti");
        return -1;
    }
    child->irq_stack_top = (uint64_t)(child->irq_stack + TASK_IRQ_STACK_SIZE);

    /* Inherit parent's fd table; bump pipe ref-counts for inherited ends. */
    for (int i = 0; i < TASK_MAX_FDS; i++) {
        child->fds[i] = parent->fds[i];
        if (child->fds[i].type == FD_PIPE_READ)
            pipe_ref_read(child->fds[i].pipe_id);
        else if (child->fds[i].type == FD_PIPE_WRITE)
            pipe_ref_write(child->fds[i].pipe_id);
    }

    /* Save parent's interrupt frame; child gets RAX=0 (fork returns 0). */
    child->fork_frame     = *frame;
    child->fork_frame.rax = 0;

    /* Build initial kernel stack for task_switch:
       task_switch pops r15,r14,r13,r12,rbx,rbp then rets to trampoline. */
    uint64_t *sp = (uint64_t*)child->kstack_top;
    *(--sp) = (uint64_t)fork_child_trampoline;
    *(--sp) = 0; /* rbp */
    *(--sp) = 0; /* rbx */
    *(--sp) = 0; /* r12 */
    *(--sp) = 0; /* r13 */
    *(--sp) = 0; /* r14 */
    *(--sp) = 0; /* r15 */
    child->rsp = (uint64_t)sp;

    if (id == task_count) task_count++;  /* only grow array for new slots */
    __asm__ volatile("sti");
    return id;
}

void sched_tick(void)
{
    if (!sched_ready) return;
    tasks[current_task].ticks++;

    int next = current_task;
    for (int i = 1; i <= task_count; i++) {
        int c = (current_task + i) % task_count;
        if (tasks[c].state == TASK_READY || tasks[c].state == TASK_RUNNING) {
            next = c; break;
        }
    }
    if (next == current_task) return;

    if (tasks[current_task].state == TASK_RUNNING)
        tasks[current_task].state = TASK_READY;
    tasks[next].state = TASK_RUNNING;

    /* Switch CR3 and TSS.rsp0 to the next task's page table / kernel stack.
       All kernel stacks live in the shared identity-mapped region so the
       CR3 switch is safe at any point during ring-0 execution. */
    uint64_t next_cr3 = tasks[next].cr3 ? tasks[next].cr3 : vmm_kernel_cr3();
    vmm_load_cr3(next_cr3);
    tss_set_rsp0(tasks[next].irq_stack_top);

    int prev    = current_task;
    current_task = next;
    task_switch(&tasks[prev].rsp, tasks[next].rsp);
}

void sched_yield(void)
{
    __asm__ volatile("cli");
    sched_tick();
    __asm__ volatile("sti");
}

task_t *sched_get_tasks(void)  { return tasks; }
int     sched_get_count(void)  { return task_count; }
int     sched_current_id(void) { return current_task; }

void sched_lock(void)   { sched_ready = 0; }
void sched_unlock(void) { sched_ready = 1; }
