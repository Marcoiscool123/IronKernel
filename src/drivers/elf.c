/* IronKernel v0.04 — elf.c  (ELF64 loader) */
#include "elf.h"
#include "fat32.h"
#include "vga.h"
#include "../kernel/types.h"
#include "../kernel/tss.h"
#include "../kernel/heap.h"
#include "../kernel/sched.h"
#include "../kernel/vmm.h"
#include "../kernel/pmm.h"

#define ELF_MAX_SIZE      (128 * 1024)
#define USER_STACK_TOP    0x701000ULL
#define USER_STACK_PAGES  4                              /* 16KB stack */
#define USER_STACK_BASE   (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

static void phex(uint64_t n)
{
    char b[17]; b[16]=0;
    for(int i=15;i>=0;i--){b[i]="0123456789ABCDEF"[n&0xF];n>>=4;}
    vga_print("0x"); vga_print(b);
}

/* Debug port 0xE9 — output visible in QEMU: -debugcon file:/tmp/ik_debug.log */
static inline void e9s(const char *s) {
    while (*s) { __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)*s)); s++; }
}
static inline void e9x(uint64_t v) {
    const char *h = "0123456789ABCDEF";
    e9s("0x");
    for (int i = 60; i >= 0; i -= 4)
        __asm__ volatile("outb %0,$0xE9"::"a"((uint8_t)h[(v>>i)&0xF]));
}
static void puint(uint64_t n)
{
    if(!n){vga_print("0");return;}
    char b[21];b[20]=0;int i=19;
    while(n&&i>=0){b[i--]='0'+(n%10);n/=10;}
    vga_print(&b[i+1]);
}


int elf_exec(const char *name, const char *ext)
{
    sched_lock();

    uint8_t *elf_buf = (uint8_t*)kmalloc(ELF_MAX_SIZE);
    if (!elf_buf) {
        vga_print("\n  ELF: out of memory\n");
        sched_unlock();
        return -1;
    }

    uint32_t bytes = 0;
    if (fat32_read_file(name, ext, elf_buf, ELF_MAX_SIZE, &bytes) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  ELF: file not found\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kfree(elf_buf);
        sched_unlock();
        return -1;
    }

    if (bytes < sizeof(Elf64Hdr)) {
        vga_print("\n  ELF: too small\n");
        kfree(elf_buf);
        sched_unlock();
        return -1;
    }

    Elf64Hdr *h = (Elf64Hdr*)elf_buf;
    if (h->ident[0]!=ELF_MAGIC0||h->ident[1]!=ELF_MAGIC1||
        h->ident[2]!=ELF_MAGIC2||h->ident[3]!=ELF_MAGIC3) {
        vga_print("\n  ELF: bad magic\n");
        kfree(elf_buf);
        sched_unlock();
        return -1;
    }
    if (h->ident[4] != ELFCLASS64) {
        vga_print("\n  ELF: not 64-bit\n");
        kfree(elf_buf);
        sched_unlock();
        return -1;
    }
    if (h->type != ET_EXEC) {
        vga_print("\n  ELF: not executable\n");
        kfree(elf_buf);
        sched_unlock();
        return -1;
    }
    if (h->machine != EM_X86_64) {
        vga_print("\n  ELF: not x86-64\n");
        kfree(elf_buf);
        sched_unlock();
        return -1;
    }

    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  ELF64 LOADER\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("    entry : "); phex(h->entry); vga_print("\n");
    vga_print("    segs  : "); puint(h->phnum); vga_print("\n");

    /* ── Create per-process page table ─────────────────────────── */
    uint64_t proc_cr3 = vmm_create_user_pt();
    if (!proc_cr3) {
        vga_print("\n  ELF: out of frames\n");
        kfree(elf_buf);
        sched_unlock();
        return -1;
    }

    /* ── Map each PT_LOAD segment into process address space ──── */
    for (uint16_t i = 0; i < h->phnum; i++) {
        Elf64Phdr *ph = (Elf64Phdr*)(elf_buf + h->phoff +
                         (uint64_t)i * h->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;

        vga_print("    load  : vaddr="); phex(ph->vaddr);
        vga_print("  sz="); puint(ph->filesz); vga_print("\n");

        uint64_t vstart = ph->vaddr & ~(PAGE_SIZE - 1);
        uint64_t vend   = (ph->vaddr + ph->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint64_t va = vstart; va < vend; va += PAGE_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) {
                vga_print("\n  ELF: out of frames during load\n");
                vmm_free_user_pt(proc_cr3);
                kfree(elf_buf);
                sched_unlock();
                return -1;
            }
            uint8_t *fp = (uint8_t*)frame;
            for (uint32_t z = 0; z < PAGE_SIZE; z++) fp[z] = 0;

            /* Copy file data that falls within this 4KB page */
            uint64_t file_start = ph->vaddr;
            uint64_t file_end   = ph->vaddr + ph->filesz;
            uint64_t page_end   = va + PAGE_SIZE;
            uint64_t cs = (va       > file_start) ? va       : file_start;
            uint64_t ce = (page_end < file_end)   ? page_end : file_end;
            if (cs < ce) {
                uint8_t *src = elf_buf + ph->offset + (cs - file_start);
                uint8_t *dst = fp + (cs - va);
                uint64_t len = ce - cs;
                for (uint64_t b = 0; b < len; b++) dst[b] = src[b];
            }

            if (vmm_map_page_in(proc_cr3, va, frame, PAGE_USER | PAGE_RW) != 0) {
                vga_print("\n  ELF: out of frames for page table\n");
                vmm_free_user_pt(proc_cr3);
                kfree(elf_buf);
                sched_unlock();
                return -1;
            }
        }
    }

    /* ── Map user stack (4 × 4KB pages below USER_STACK_TOP) ─── */
    for (uint64_t va = USER_STACK_BASE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) {
            vga_print("\n  ELF: out of frames for stack\n");
            vmm_free_user_pt(proc_cr3);
            kfree(elf_buf);
            sched_unlock();
            return -1;
        }
        uint8_t *fp = (uint8_t*)frame;
        for (uint32_t z = 0; z < PAGE_SIZE; z++) fp[z] = 0;
        if (vmm_map_page_in(proc_cr3, va, frame, PAGE_USER | PAGE_RW) != 0) {
            vga_print("\n  ELF: out of frames for stack page table\n");
            vmm_free_user_pt(proc_cr3);
            kfree(elf_buf);
            sched_unlock();
            return -1;
        }
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("    jumping to user mode...\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    uint64_t entry    = h->entry;
    uint64_t kern_cr3 = vmm_kernel_cr3();
    int      cur      = sched_current_id();

    kfree(elf_buf);

    /* Store proc_cr3 in this task so the scheduler restores it correctly
       if an IRQ fires while the user process is running. */
    task_t *tasks = sched_get_tasks();
    e9s("\n[ELF] A: before vmm_load_cr3(proc) irqstack="); e9x(tasks[cur].irq_stack_top); e9s("\n");
    tasks[cur].cr3 = proc_cr3;
    tss_set_rsp0(tasks[cur].irq_stack_top);  /* ring-3 interrupts land on separate stack */
    vmm_load_cr3(proc_cr3);           /* switch to process address space */
    e9s("[ELF] B: after vmm_load_cr3(proc)\n");

    /* Unlock the scheduler NOW so fork children can be scheduled
       by PIT ticks while this process runs in user mode. */
    sched_unlock();

    __asm__ volatile("sti");          /* allow scheduler to preempt user code */
    e9s("[ELF] C: about to enter user mode entry="); e9x(entry); e9s("\n");
    user_mode_enter(entry, USER_STACK_TOP);
    /* Returns here after SYS_EXIT — interrupts may be off */
    e9s("[ELF] D: returned from user mode\n");
    __asm__ volatile("cli");

    /* Relock for cleanup — prevents scheduler from running during teardown. */
    sched_lock();

    /* tasks[cur].cr3 may differ from proc_cr3 if the user program called
       SYS_EXEC: SYS_EXEC already freed proc_cr3 and updated tasks[cur].cr3
       to the new process's CR3 (which is what we must free now).
       Always free tasks[cur].cr3 to avoid a double-free of proc_cr3. */
    uint64_t current_user_cr3 = tasks[cur].cr3;
    tasks[cur].cr3 = kern_cr3;
    e9s("[ELF] E: before vmm_load_cr3(kern)\n");
    vmm_load_cr3(kern_cr3);
    e9s("[ELF] F: before vmm_free_user_pt\n");
    vmm_free_user_pt(current_user_cr3);   /* reclaim all process frames */
    e9s("[ELF] G: after vmm_free_user_pt\n");

    sched_unlock();
    __asm__ volatile("sti");          /* re-enable interrupts for shell hlt loop */

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("    process returned.\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    e9s("[ELF] H: elf_exec done\n");
    return 0;
}
