#ifndef PANIC_H
#define PANIC_H
#include "types.h"

/* ── KERNEL PANIC SUBSYSTEM ──────────────────────────────────────────────
   panic_ex  — low-level panic entry; called by exception handlers.
   panic()   — macro for use anywhere in kernel code.
   ASSERT()  — halting assertion check.
   ─────────────────────────────────────────────────────────────────────── */

/* Full panic with exception context (called from isr_handler) */
void panic_ex(const char *msg,
              uint64_t rip, uint64_t rsp, uint64_t rbp,
              uint64_t err, uint64_t cr2,
              uint32_t intnum,   /* 0xFFFFFFFF = not from exception */
              int      is_user); /* 1 = fault came from ring-3 code */

/* Panic from arbitrary kernel code — captures current register state. */
#define panic(msg)  do {                                              \
    uint64_t _rip, _rsp, _rbp;                                       \
    __asm__ volatile("lea 1f(%%rip),%%rax; mov %%rax,%0\n1:"         \
                     :"=m"(_rip)::"rax");                             \
    __asm__ volatile("mov %%rsp,%0":"=m"(_rsp));                      \
    __asm__ volatile("mov %%rbp,%0":"=m"(_rbp));                      \
    panic_ex((msg), _rip, _rsp, _rbp, 0, 0, 0xFFFFFFFFu, 0);        \
} while(0)

/* Halting assertion — panics with source location if cond is false. */
#define ASSERT(cond)  do {                                            \
    if (!(cond)) panic("ASSERT failed: " #cond                       \
                       "  (" __FILE__ ":" _PANIC_STR(__LINE__) ")"); \
} while(0)

#define _PANIC_STR(x)  _PANIC_STR2(x)
#define _PANIC_STR2(x) #x

#endif
