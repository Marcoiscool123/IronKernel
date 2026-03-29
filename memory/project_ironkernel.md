---
name: IronKernel project state
description: IronKernel v0.04 architecture, current state, and key bugs fixed
type: project
---

# IronKernel v0.04 — Project Overview

Freestanding x86-64 kernel. Boots via GRUB multiboot2. VBE 800×600×32 framebuffer.

## Architecture
- **Boot**: NASM boot.asm → multiboot2 header, long mode setup
- **Memory**: PMM (bitmap), VMM (4-level paging, huge pages for kernel), heap (kmalloc/kfree)
- **Scheduler**: preemptive round-robin, PIT IRQ0 at 100Hz, task_switch in switch.asm
- **Syscalls**: int 0x80, handlers in syscall.c
- **Drivers**: VGA/VBE, PS/2 keyboard, PS/2 mouse, ATA, FAT32, PCI, e1000 NIC
- **ELF loader**: elf.c loads ELF64 executables into per-process page tables
- **WM**: floating window manager in wm.c, 800×600 VBE, mouse cursor, taskbar

## Key design facts
- Kernel identity-mapped with 2MB huge pages (PML4[0] → PD with PT_LARGE entries)
- User processes get per-process PML4 (vmm_create_user_pt copies kernel PD, strips USER bit)
- ELF loads at 0x200000; stack at 0x6F1000–0x701000
- Tasks have separate `irq_stack` (4KB) for ring-3→ring-0 transitions (TSS.rsp0 = irq_stack_top)
- `wm_elf_out_win` global routes vga_print to ELF window explicitly (no sched_current_id race)

## Major bugs fixed this session
1. **IRQ stack overwrite**: TSS.rsp0 pointed to kstack_top. Ring-3 interrupts overwrote elf_exec's stack frame (kern_cr3 local). Fix: per-task irq_stack, tss_set_rsp0(irq_stack_top).
2. **SYS_EXEC same bug**: SYS_EXEC also used kstack_top. Fixed to irq_stack_top.
3. **Output routing race**: wm_hook_print used sched_current_id() which races with PIT. Fixed with explicit wm_elf_out_win global set by elf_task_trampoline.
4. **Mouse bounds**: mouse.c clamped to 639×479 (640×480 era). Fixed to 799×599.
5. **Keyboard split**: WM loop and ELF's SYS_READ both consumed from same keyboard buffer. Fixed: WM skips keyboard entirely when elf_running=1.
6. **Backspace in WM**: SYS_READ called vga_backspace() (direct VGA, bypasses WM). Fixed: vga_print("\b") + wm_putchar handles '\b'.
7. **elf_running not cleared**: Clicking X on ELF window didn't clear elf_running. Fixed in handle_mouse close button handler.
8. **Output leak to shell**: After ELF window closed (alive=0), ELF cleanup prints fell through to shell. Fixed: tasks with dead win_id get output discarded.

## WM features added
- Taskbar window buttons (TBTN_W=90, starts at x=172) — click to focus
- ELF programs open in 554×546 windows (same as shell), cascade 20px
- PS command shows WIN column with window title and * for focused
- Partial clock update (800×20 rows) instead of full 800×600 redraw each second
- Fast drawing: vga_rect/hline/gradient/blit_char use direct row-pointer arithmetic

**Why:** All bugs relate to incorrect assumptions about kernel stack layout and scheduler state during user-mode ELF execution.
**How to apply:** Any future user-mode execution feature must use irq_stack for TSS.rsp0, and must use explicit globals (not sched_current_id) for routing output in interrupt-preemptible contexts.
