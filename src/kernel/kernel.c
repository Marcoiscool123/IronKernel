/* IronKernel v0.03 — kernel.c */
#include "types.h"
#include "../drivers/vga.h"
#include "gdt.h"
#include "idt.h"
#include "tss.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "../drivers/pit.h"
#include "../drivers/keyboard.h"
#include "sched.h"
#include "pipe.h"
#include "shell.h"
#include "scroll.h"
#include "../drivers/ata.h"
#include "../drivers/fat12.h"
#include "../drivers/fat32.h"
#include "../drivers/pci.h"
#include "../drivers/e1000.h"
#include "../drivers/dhcp.h"
#include "../drivers/ip.h"
#include "../drivers/ac97.h"
#include "klog.h"
#include "../drivers/serial.h"

/* Ring-0 stack: used by TSS rsp0 during user-mode interrupts.
   65536 bytes = 64 KB. Declared here, top passed to tss_init(). */
static uint8_t ik_ring0_stack[65536] __attribute__((aligned(16)));
#define RING0_STACK_TOP ((uint64_t)(ik_ring0_stack + sizeof(ik_ring0_stack)))


/* ── Multiboot2 framebuffer tag parser ──────────────────────────────────── */
/* Scans the multiboot2 info structure for the framebuffer tag (type 8).
   If found, calls vga_set_fb() so vga_init() uses the VBE LFB. */
static void find_fb_tag(uint64_t mb_info)
{
    /* MB2 info block: 8-byte header (total_size u32, reserved u32),
       followed by a sequence of variable-length tags aligned to 8 bytes. */
    uint8_t *p = (uint8_t*)(mb_info + 8);
    for (;;) {
        uint32_t type = *(uint32_t*)p;
        uint32_t size = *(uint32_t*)(p + 4);
        if (type == 0) break;          /* terminator tag */
        if (type == 8) {               /* framebuffer info tag */
            uint64_t addr  = *(uint64_t*)(p + 8);
            uint32_t pitch = *(uint32_t*)(p + 16);
            /* Only use VBE if addr falls in the high region we mapped
               in boot.asm (pd_table_high: 0xC0000000-0xFFFFFFFF).
               Addresses below this would cause a page fault. */
            if (addr >= 0xC0000000 && addr < 0x100000000ULL)
                vga_set_fb(addr, pitch);
            break;
        }
        /* Tags are padded to 8-byte alignment */
        p += (size + 7) & ~7u;
    }
}

/* ── Kernel entry ───────────────────────────────────────────────────────── */
void kernel_main(uint64_t mb_magic, uint64_t mb_info)
{
    (void)mb_magic;
    serial_init();
    serial_puts("[SERIAL] COM1 ready at 115200 baud\r\n");
    find_fb_tag(mb_info);
    vga_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("IronKernel v0.03  [x86_64 Long Mode]\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    tss_init(RING0_STACK_TOP);
    gdt_init();
    vga_print("[GDT]   64-bit GDT + TSS loaded\n");
    klog(LOG_INFO, "[GDT] 64-bit GDT + TSS loaded");

    idt_init();
    vga_print("[IDT]   64-bit IDT loaded, PIC remapped\n");
    klog(LOG_INFO, "[IDT] 64-bit IDT loaded, PIC remapped to IRQ 32-47");

    pmm_init(mb_info);
    vga_print("[PMM]   physical memory manager ready\n");
    klog(LOG_INFO, "[PMM] physical memory manager ready");

    vmm_init();
    vga_print("[VMM]   4-level paging active (1 GB identity map)\n");
    klog(LOG_INFO, "[VMM] 4-level paging active, 1 GB identity map");

    heap_init();
    vga_print("[HEAP]  kernel heap ready\n");
    klog(LOG_INFO, "[HEAP] kernel heap ready");

    pit_init();
    vga_print("[PIT]   100 Hz timer\n");
    klog(LOG_INFO, "[PIT] channel 0 @ 100 Hz");

    keyboard_init();
    vga_print("[KB]    PS/2 keyboard ready\n");
    klog(LOG_INFO, "[KB] PS/2 keyboard initialised");

    scroll_init();
    ata_init();   /* ata_init logs its own messages via klog */

    if (fat32_init() == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_print("[FAT32] mounted OK\n");
        klog(LOG_INFO, "[FAT32] volume mounted OK");
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("[FAT32] no FAT32 volume\n");
        klog(LOG_WARN, "[FAT32] no FAT32 volume found");
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    __asm__ volatile("sti");

    pipe_init();
    klog(LOG_INFO, "[PIPE] pipe subsystem ready");

    sched_init();
    vga_print("[SCHED] scheduler ready\n");
    klog(LOG_INFO, "[SCHED] round-robin scheduler ready");

    pci_init();
    vga_print("PCI initialized\n");
    klog(LOG_INFO, "[PCI] bus enumeration complete");

    ac97_init();   /* ac97_init prints its own vga message */
    if (ac97_detected())
        klog(LOG_INFO, "[AC97] Intel AC97 audio controller ready");
    else
        klog(LOG_WARN, "[AC97] no audio device found");

    e1000_init();
    vga_print("E1000 ready\n");
    klog(LOG_INFO, "[E1000] Intel 82540EM NIC ready");

    vga_print("[DHCP] requesting address...\n");
    if (dhcp_discover() == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_print("[DHCP] ");
        /* Build IP string for klog */
        static char dhcp_msg[48];
        static const char dhcp_pfx[] = "[DHCP] assigned ";
        int di = 0;
        for (; dhcp_pfx[di]; di++) dhcp_msg[di] = dhcp_pfx[di];
        for (int i = 0; i < 4; i++) {
            uint8_t v = g_net_ip[i];
            if (v >= 100) { vga_print((char[]){(char)('0'+v/100),0}); dhcp_msg[di++]=(char)('0'+v/100); }
            if (v >=  10) { vga_print((char[]){(char)('0'+(v/10)%10),0}); dhcp_msg[di++]=(char)('0'+(v/10)%10); }
            { vga_print((char[]){(char)('0'+v%10),0}); dhcp_msg[di++]=(char)('0'+v%10); }
            if (i < 3) { vga_print("."); dhcp_msg[di++]='.'; }
        }
        dhcp_msg[di] = '\0';
        vga_print("\n");
        klog(LOG_INFO, dhcp_msg);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("[DHCP] timeout — using 10.0.2.15\n");
        klog(LOG_WARN, "[DHCP] timeout — using static 10.0.2.15");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }

    shell_run();

    for(;;) __asm__ volatile("hlt");
}
