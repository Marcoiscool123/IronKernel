/* IronKernel v0.04 — idt.c
   Builds the 256-entry 64-bit IDT, remaps the PIC, installs all gates. */
#include "idt.h"
#include "gdt.h"
#include "types.h"
#include "../drivers/vga.h"
#include "syscall.h"
#include "panic.h"
#include "tss.h"
#include "vmm.h"
#include "klog.h"

static inline void outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static inline void io_wait(void) { outb(0x80, 0); }

/* ── IDT storage ─────────────────────────────────────────────────────── */
static IDTEntry idt[256];

typedef struct __attribute__((packed)) { uint16_t limit; uint64_t base; } IDTR;
static IDTR idtr;

/* ── IRQ soft-dispatch table ─────────────────────────────────────────── */
static void (*irq_dispatch[16])(void);

/* ── Gate encoder ────────────────────────────────────────────────────── */
static void idt_set(uint8_t n, void (*fn)(void), uint8_t attr)
{
    uint64_t addr = (uint64_t)fn;
    idt[n].offset_low  = addr & 0xFFFF;
    idt[n].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[n].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[n].selector    = GDT_KERNEL_CODE;
    idt[n].ist         = 0;
    idt[n].type_attr   = attr;
    idt[n].reserved    = 0;
}

/* ── PIC remap ───────────────────────────────────────────────────────── */
static void pic_remap(void)
{
    /* Remap master→32, slave→40 to avoid collision with CPU exceptions */
    outb(0x20,0x11); io_wait(); outb(0xA0,0x11); io_wait();
    outb(0x21,0x20); io_wait(); outb(0xA1,0x28); io_wait();
    outb(0x21,0x04); io_wait(); outb(0xA1,0x02); io_wait();
    outb(0x21,0x01); io_wait(); outb(0xA1,0x01); io_wait();
    outb(0x21,0x00); /* unmask all master IRQs */
    outb(0xA1,0x00); /* unmask all slave  IRQs */
}

/* ── IDT init ────────────────────────────────────────────────────────── */
void idt_init(void)
{
    pic_remap();

    void (*isrs[32])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10,isr11,isr12,isr13,isr14,isr15,
        isr16,isr17,isr18,isr19,isr20,isr21,isr22,isr23,
        isr24,isr25,isr26,isr27,isr28,isr29,isr30,isr31
    };
    for (int i = 0; i < 32; i++)
        idt_set(i, isrs[i], IDT_GATE_INT);

    void (*irqs[16])(void) = {
        irq0,irq1,irq2,irq3,irq4,irq5,irq6,irq7,
        irq8,irq9,irq10,irq11,irq12,irq13,irq14,irq15
    };
    for (int i = 0; i < 16; i++)
        idt_set(32 + i, irqs[i], IDT_GATE_INT);

    /* syscall gate DPL=3 — use syscall_stub (idt.asm) so all syscalls
       route through isr_handler → syscall_dispatch in syscall.c.
       The old syscall_stub_64 only handled SYS_WRITE and SYS_EXIT;
       syscalls 2-6 silently fell through. */
    idt_set(0x80, syscall_stub, IDT_GATE_USER);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));
    /* lidt can be done inline in C with a memory operand in 64-bit mode —
       no separate idt_flush asm stub needed unlike in 32-bit. */
}

/* ── Exception names ─────────────────────────────────────────────────── */
static const char *exc_names[32] = {
    "Divide-by-Zero","Debug","NMI","Breakpoint","Overflow","Bound Range",
    "Invalid Opcode","Device Not Available","Double Fault",
    "Coprocessor Overrun","Invalid TSS","Segment Not Present",
    "Stack-Segment Fault","General Protection Fault","Page Fault","Reserved",
    "x87 FP","Alignment Check","Machine Check","SIMD FP",
    "Virtualization","Control Protection",
    "#22","#23","#24","#25","#26","#27","#28","#29","Security","#31"
};

static inline void e9_hex64(uint64_t v)
{
    const char *h = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4)
        __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)h[(v>>i)&0xF]));
}
static inline void e9_str(const char *s)
{
    while (*s) { __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)*s)); s++; }
}

/* ── ISR handler (called from idt.asm isr_common) ────────────────────── */
void isr_handler(InterruptFrame *frame)
{
    if (frame->int_num == 0x80) {
        syscall_dispatch(frame);   /* dispatch to syscall.c */
        return;
    }

    /* Dump to debug console before touching the framebuffer. */
    e9_str("\n!EXC int="); e9_hex64(frame->int_num);
    e9_str(" err="); e9_hex64(frame->err_code);
    e9_str(" rip="); e9_hex64(frame->rip);
    e9_str("\n");

    /* Read CR2 (page-fault linear address). */
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    const char *name = (frame->int_num < 32)
                       ? exc_names[frame->int_num]
                       : "Unknown Exception";

    if (frame->cs & 3) {
        /* ── Ring-3 fault: ELF crash — kill the task, don't panic ── */
        char buf[17]; buf[16] = 0;
        uint64_t v;

        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n[ELF CRASH] ");
        vga_print(name);
        vga_print("  RIP=0x");
        v = frame->rip;
        for (int i = 15; i >= 0; i--) { buf[i] = "0123456789ABCDEF"[v&0xF]; v >>= 4; }
        vga_print(buf);
        if (frame->int_num == 14) {   /* #PF: show CR2 */
            vga_print("  CR2=0x");
            v = cr2;
            for (int i = 15; i >= 0; i--) { buf[i] = "0123456789ABCDEF"[v&0xF]; v >>= 4; }
            vga_print(buf);
        }
        vga_print("\n");

        /* Log crash to dmesg ring buffer */
        {
            static char klog_crash[80];
            static const char cpfx[] = "[ELF CRASH] ";
            int ci = 0;
            for (; cpfx[ci]; ci++) klog_crash[ci] = cpfx[ci];
            for (int j = 0; name[j] && ci < 72; j++) klog_crash[ci++] = name[j];
            static const char crip[] = "  RIP=0x";
            for (int j = 0; crip[j] && ci < 72; j++) klog_crash[ci++] = crip[j];
            v = frame->rip;
            for (int i = 15; i >= 0 && ci < 78; i--)
                klog_crash[ci++] = "0123456789ABCDEF"[(v >> (i*4)) & 0xF];
            klog_crash[ci] = '\0';
            klog(LOG_ERROR, klog_crash);
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        /* Return to shell/WM — same path as normal SYS_EXIT.
           wm_spawn_elf / cmd_exec cleans up elf_running and windows
           automatically when kernel_exit_restore() returns to them. */
        vmm_load_cr3(vmm_kernel_cr3());
        kernel_exit_restore();
        __builtin_unreachable();
    }

    /* ── Ring-0 fault: genuine kernel bug — full panic ── */
    panic_ex(name,
             frame->rip, frame->rsp, frame->rbp,
             frame->err_code, cr2,
             (uint32_t)frame->int_num,
             0);
}

/* ── IRQ handler (called from irq.asm irq_common) ───────────────────── */
void irq_handler(uint32_t irq_num)
{
    /* Send EOI BEFORE dispatch.
       sched_tick (called from the PIT handler) may perform a task switch
       via task_switch whose return path goes to fork_child_trampoline
       instead of back here — fork_child_trampoline calls iretq directly,
       so this function would never send EOI.  With the master-PIC IRQ0
       ISR bit left set, the PIC blocks all further IRQ delivery (including
       IRQ1 keyboard) until a later switch that does return through here.
       Sending EOI first is safe: IF is still 0 (interrupt gate cleared it),
       so the PIC cannot re-deliver the same IRQ before we finish. */
    if (irq_num >= 8) outb(0xA0, 0x20); /* slave  EOI (IRQ8-15) */
    outb(0x20, 0x20);                    /* master EOI */

    if (irq_num < 16 && irq_dispatch[irq_num])
        irq_dispatch[irq_num]();
}

/* ── irq_install — called by pit_init, keyboard_init ────────────────── */
void irq_install(uint8_t irq, void (*handler)(void))
{
    if (irq < 16)
        irq_dispatch[irq] = handler;
}
