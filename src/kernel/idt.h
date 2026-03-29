/* IronKernel v0.04 — idt.h
   64-bit IDT: each entry is 16 bytes (handler address is 64 bits). */
#ifndef IDT_H
#define IDT_H
#include "types.h"

/* 16-byte IDT gate descriptor */
typedef struct __attribute__((packed)) {
    uint16_t offset_low;   /* handler bits 15:0  */
    uint16_t selector;     /* code segment (GDT_KERNEL_CODE = 0x08) */
    uint8_t  ist;          /* IST index (0 = use current RSP) */
    uint8_t  type_attr;    /* gate type + DPL + present */
    uint16_t offset_mid;   /* handler bits 31:16 */
    uint32_t offset_high;  /* handler bits 63:32 */
    uint32_t reserved;     /* must be zero */
} IDTEntry;

/* Gate type_attr values */
#define IDT_GATE_INT   0x8E   /* P=1 DPL=0 type=0xE interrupt gate */
#define IDT_GATE_USER  0xEE   /* P=1 DPL=3 type=0xE (int 0x80) */

/* Interrupt frame — matches the stack layout built by idt.asm stubs.
   After 15 GPR pushes, RSP points here. int_num at offset 120.    */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax; /* [0..112] */
    uint64_t int_num;    /* [120] pushed by stub macro */
    uint64_t err_code;   /* [128] pushed by stub or CPU */
    uint64_t rip;        /* [136] CPU iretq frame begins */
    uint64_t cs;         /* [144] */
    uint64_t rflags;     /* [152] */
    uint64_t rsp;        /* [160] user RSP (only valid on ring-change) */
    uint64_t ss;         /* [168] */
} InterruptFrame;

void idt_init(void);

/* C-level handlers called from asm stubs */
void isr_handler(InterruptFrame *frame); /* exceptions + syscall */
void irq_handler(uint32_t irq_num);     /* hardware IRQs */

/* ISR stubs — defined in boot/idt.asm */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void syscall_stub(void);

/* IRQ stubs — defined in boot/irq.asm */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

#endif
void irq_install(uint8_t irq, void (*handler)(void));
