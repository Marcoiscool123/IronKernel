#include "shell.h"
#include "types.h"
#include "wm.h"
#include "pmm.h"
#include "heap.h"
#include "../drivers/vga.h"
#include "../drivers/keyboard.h"
#include "../drivers/pit.h"
#include "../drivers/ata.h"
#include "../drivers/fat32.h"
#include "../drivers/elf.h"
#include "../drivers/pci.h"
#include "../drivers/e1000.h"
#include "../drivers/arp.h"
#include "../drivers/ip.h"
#include "../drivers/tcp.h"
#include "../drivers/dns.h"
#include "../drivers/dhcp.h"
#include "scroll.h"
#include "sched.h"
#include "klog.h"
#include "../drivers/speaker.h"
#include "../drivers/ac97.h"
#include "klog.h"
#include "browser.h"



static uint32_t parse_uint(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static void parse_ip(const char **args, uint8_t *ip) {
    for (int i = 0; i < 4; i++) {
        ip[i] = (uint8_t)parse_uint(*args);
        while (**args >= '0' && **args <= '9') (*args)++;
        if (i < 3 && **args == '.') (*args)++;
    }
}
/* ── STRING HELPERS ─────────────────────────────────────────────────
   No libc. We implement only what we need.
   ─────────────────────────────────────────────────────────────── */

static size_t str_len(const char* s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int str_eq(const char* a, const char* b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
    /* Returns 1 if strings are identical, 0 otherwise. */
}

static void print_uint(uint32_t n)
{
    if (n == 0) { vga_print("0"); return; }
    char buf[12]; buf[11] = '\0';
    int i = 10;
    while (n > 0 && i >= 0) {
        buf[i--] = '0' + (n % 10);
        n /= 10;
    }
    vga_print(&buf[i + 1]);
}

/* Returns 1 if s looks like a hostname (contains a letter or hyphen),
   0 if it is a raw IPv4 dotted-decimal string (only digits and dots). */
static int is_hostname(const char *s)
{
    for (; *s && *s != '/'; s++)
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '-')
            return 1;
    return 0;
}

/* Resolve a host string (hostname or dotted-decimal) into ip[4].
   If DNS returns multiple addresses, pings each and picks the fastest.
   Returns 0 on success, -1 if DNS lookup failed. */
static int resolve_host(const char *host, uint8_t ip[4])
{
    /* Strip http:// or https:// so callers don't have to */
    if (host[0]=='h' && host[1]=='t' && host[2]=='t' && host[3]=='p') {
        while (*host && *host != '/') host++;
        if (*host == '/') host++;
        if (*host == '/') host++;
    }

    if (!is_hostname(host)) {
        const char *ptr = host;
        parse_ip(&ptr, ip);
        return 0;
    }

    /* Extract bare hostname (stop at ':' or '/') */
    char hbuf[128];
    int  hi = 0;
    for (; host[hi] && host[hi] != '/' && host[hi] != ':' && hi < 127; hi++)
        hbuf[hi] = host[hi];
    hbuf[hi] = '\0';

    vga_print("  Resolving "); vga_print(hbuf); vga_print("...\n");

    uint8_t list[DNS_MAX_ADDRS][4];
    int count = dns_resolve_all(hbuf, list, DNS_MAX_ADDRS);
    if (count <= 0) {
        vga_print("  DNS: could not resolve host.\n");
        return -1;
    }

    if (count == 1) {
        /* Only one address — use it directly */
        for (int i = 0; i < 4; i++) ip[i] = list[0][i];
        vga_print("  -> ");
        for (int i = 0; i < 4; i++) { print_uint(ip[i]); if (i < 3) vga_print("."); }
        vga_print("\n");
        return 0;
    }

    /* Multiple addresses — ping each, pick fastest */
    int best_idx = 0;
    int best_rtt = -1;
    for (int a = 0; a < count; a++) {
        vga_print("  Probing ");
        for (int i = 0; i < 4; i++) { print_uint(list[a][i]); if (i < 3) vga_print("."); }
        vga_print("... ");
        int rtt = icmp_ping(list[a], (uint16_t)(a + 1));
        if (rtt >= 0) {
            print_uint((uint32_t)rtt); vga_print(" ms\n");
            if (best_rtt < 0 || rtt < best_rtt) { best_rtt = rtt; best_idx = a; }
        } else {
            vga_print("timeout\n");
        }
    }

    for (int i = 0; i < 4; i++) ip[i] = list[best_idx][i];
    vga_print("  Using ");
    for (int i = 0; i < 4; i++) { print_uint(ip[i]); if (i < 3) vga_print("."); }
    if (best_rtt >= 0) { vga_print(" ("); print_uint((uint32_t)best_rtt); vga_print(" ms)"); }
    vga_print("\n");
    return 0;
}

/* ── HISTORY ────────────────────────────────────────────────────── */
#define HIST_SIZE   16
#define HIST_MAX    256

static char hist_buf[HIST_SIZE][HIST_MAX];
static int  hist_count = 0;
static int  hist_tail  = 0;

static void hist_push(const char *cmd)
{
    if (!*cmd) return;
    if (hist_count > 0) {
        int prev = (hist_tail - 1 + HIST_SIZE) % HIST_SIZE;
        if (str_eq(hist_buf[prev], cmd)) return;
    }
    int i = 0;
    while (cmd[i] && i < HIST_MAX - 1) { hist_buf[hist_tail][i] = cmd[i]; i++; }
    hist_buf[hist_tail][i] = '\0';
    hist_tail = (hist_tail + 1) % HIST_SIZE;
    if (hist_count < HIST_SIZE) hist_count++;
}

static const char *hist_get(int back)
{
    if (back < 1 || back > hist_count) return (void*)0;
    int idx = (hist_tail - back + HIST_SIZE * 2) % HIST_SIZE;
    return hist_buf[idx];
}

/* ── LINE EDITOR STATE ──────────────────────────────────────────── */
static int sh_pr_row = 0, sh_pr_col = 0;

/* ── PROMPT ─────────────────────────────────────────────────────── */

static void shell_prompt(void)
{
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("\nIK");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_print("@");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("ironkernel");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_print(":");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print(fat32_cwd);
    /* fat32_cwd starts as "/" and is updated by fat32_chdir.
       Always shown so the user always knows where they are. */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print(" > ");
    vga_get_cursor(&sh_pr_row, &sh_pr_col);
}

/* ── BUILT-IN COMMANDS ──────────────────────────────────────────── */

/* ── port helpers for power commands ──────────────── PORT I/O ─── */
static inline void shell_outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline void shell_outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

static void cmd_shutdown(void)
{
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_print("\n  Shutting down...\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    /* QEMU/Bochs ACPI power-off — try both known ports. */
    shell_outw(0x604, 0x2000);   /* QEMU ≥ 2.x ACPI shutdown  */
    shell_outw(0xB004, 0x2000);  /* Bochs / older QEMU         */
    /* If ACPI didn't fire, halt so the machine at least stops. */
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

static void cmd_reboot(void)
{
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("\n  Rebooting...\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    /* Pulse the reset line via the PS/2 keyboard controller. */
    shell_outb(0x64, 0xFE);
    /* If the pulse didn't take, triple-fault via a null IDT. */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idt = {0, 0};
    __asm__ volatile("lidt %0\n\t int $0" :: "m"(idt));
    for (;;) __asm__ volatile("hlt");
}

static void cmd_halt(void)
{
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_print("\n  System halted. Power off manually.\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

static void cmd_clear(void)
{
    vga_init();
    scroll_reset();
    /* Re-initialise VGA — clears screen, resets cursor to 0,0. */
}

static void cmd_version(void)
{
    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  " SHELL_NAME " v" SHELL_VERSION);
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_print("  //  " SHELL_ARCH "  //  " SHELL_YEAR);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("\n");
}

static void cmd_dmesg(void)
{
    vga_print("\n");
    klog_dump();
}


static void cmd_beep(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        vga_print("  usage: beep FREQ MS\n");
        return;
    }
    const char *ptr = args;
    uint32_t freq = parse_uint(ptr);
    while (*ptr && *ptr != ' ') ptr++;
    while (*ptr == ' ') ptr++;
    uint32_t ms = parse_uint(ptr);
    if (!ms) ms = 200;
    speaker_beep(freq, ms);
}

static void cmd_play(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        vga_print("  usage: play FILE.WAV\n");
        return;
    }
    if (!ac97_detected()) {
        vga_print("  no AC97 audio device (try: beep)\n");
        return;
    }
    /* Parse filename.ext into space-padded 8.3 */
    char name[9], ext[4];
    int i;
    for (i = 0; i < 8; i++) name[i] = ' '; name[8] = '\0';
    for (i = 0; i < 3; i++) ext[i]  = ' '; ext[3]  = '\0';

    i = 0;
    while (args[i] && args[i] != '.' && args[i] != ' ' && i < 8) {
        char c = args[i];
        name[i++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    const char *p = args;
    while (*p && *p != '.') p++;
    if (*p == '.') {
        p++;
        for (i = 0; i < 3 && *p && *p != ' '; i++, p++) {
            char c = *p;
            ext[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
    }

    vga_print("  [AC97] playing...\n");
    if (ac97_play_file(name, ext) != 0)
        vga_print("  [AC97] error (check format: 16-bit PCM WAV)\n");
    else
        vga_print("  [AC97] done\n");
}

static void cmd_uptime(void)
{
    uint32_t ticks = pit_get_ticks();
    uint32_t secs  = ticks / 100;
    uint32_t mins  = secs  / 60;
    uint32_t hrs   = mins  / 60;
    secs %= 60; mins %= 60;

    vga_print("\n  Uptime : ");
    print_uint(hrs);  vga_print("h ");
    print_uint(mins); vga_print("m ");
    print_uint(secs); vga_print("s");
    vga_print("  (");
    print_uint(ticks);
    vga_print(" ticks @ 100Hz)\n");
}

static void cmd_mem(void)
{
    uint32_t total = pmm_get_total_frames() * 4;
    uint32_t free  = pmm_get_free_frames()  * 4;
    uint32_t used  = total - free;

    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  Memory\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("    Total  : "); print_uint(total); vga_print(" KB\n");
    vga_print("    Used   : "); print_uint(used);  vga_print(" KB\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("    Free   : "); print_uint(free);  vga_print(" KB\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_memstat(void) { heap_memstat(); }

static void cmd_leaks(void) { heap_dump_leaks(); }

/* memdump <hex_addr> <count> — hexdump count bytes at virtual address addr */
static uint32_t parse_hex(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    while (*s) {
        char c = *s++;
        if (c >= '0' && c <= '9')      v = v * 16 + (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (uint32_t)(c - 'A' + 10);
        else break;
    }
    return v;
}

static void cmd_memdump(const char *args)
{
    static const char hx[] = "0123456789ABCDEF";
    uint32_t addr  = parse_hex(args);
    while (*args && *args != ' ') args++;
    while (*args == ' ') args++;
    uint32_t count = (*args) ? parse_uint(args) : 64;
    if (count == 0 || count > 512) count = 64;

    vga_print("\n");
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    for (uint32_t row = 0; row < count; row += 16) {
        /* address */
        char abuf[11]; abuf[0]='0'; abuf[1]='x'; abuf[10]='\0';
        uint32_t a = addr + row;
        for (int i = 9; i >= 2; i--) { abuf[i] = hx[a & 0xF]; a >>= 4; }
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_print(abuf);
        vga_print(": ");

        /* hex bytes */
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        for (uint32_t col = 0; col < 16; col++) {
            if (row + col < count) {
                uint8_t byte = p[row + col];
                char hbuf[3]; hbuf[0] = hx[byte >> 4]; hbuf[1] = hx[byte & 0xF]; hbuf[2] = '\0';
                vga_print(hbuf);
                vga_print(" ");
            } else {
                vga_print("   ");
            }
        }

        /* ascii */
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_print(" ");
        for (uint32_t col = 0; col < 16 && row + col < count; col++) {
            uint8_t byte = p[row + col];
            char cbuf[2]; cbuf[1] = '\0';
            cbuf[0] = (byte >= 0x20 && byte < 0x7F) ? (char)byte : '.';
            vga_print(cbuf);
        }
        vga_print("\n");
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ── INFO ───────────────────────────────────────────────────────────── */

static void cmd_info(void)
{
    uint32_t eax, ebx, ecx, edx;

    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  System Information\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* ── CPU vendor string ── */
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0u), "c"(0u));
    char vendor[13];
    vendor[ 0]=(char)(ebx       &0xFF); vendor[ 1]=(char)((ebx>> 8)&0xFF);
    vendor[ 2]=(char)((ebx>>16) &0xFF); vendor[ 3]=(char)((ebx>>24)&0xFF);
    vendor[ 4]=(char)(edx       &0xFF); vendor[ 5]=(char)((edx>> 8)&0xFF);
    vendor[ 6]=(char)((edx>>16) &0xFF); vendor[ 7]=(char)((edx>>24)&0xFF);
    vendor[ 8]=(char)(ecx       &0xFF); vendor[ 9]=(char)((ecx>> 8)&0xFF);
    vendor[10]=(char)((ecx>>16) &0xFF); vendor[11]=(char)((ecx>>24)&0xFF);
    vendor[12]='\0';
    uint32_t max_basic = eax;

    /* ── CPU brand string (CPUID 0x80000002-0x80000004) ── */
    uint32_t brand_raw[13]; /* 12 registers × 4 bytes = 48 bytes + null word */
    brand_raw[12] = 0;
    char *brand = (char *)brand_raw;
    __asm__ volatile("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000000u), "c"(0u));
    uint32_t max_ext = eax;
    if (max_ext >= 0x80000004u) {
        __asm__ volatile("cpuid"
            : "=a"(brand_raw[0]),"=b"(brand_raw[1]),"=c"(brand_raw[2]),"=d"(brand_raw[3])
            : "a"(0x80000002u),"c"(0u));
        __asm__ volatile("cpuid"
            : "=a"(brand_raw[4]),"=b"(brand_raw[5]),"=c"(brand_raw[6]),"=d"(brand_raw[7])
            : "a"(0x80000003u),"c"(0u));
        __asm__ volatile("cpuid"
            : "=a"(brand_raw[8]),"=b"(brand_raw[9]),"=c"(brand_raw[10]),"=d"(brand_raw[11])
            : "a"(0x80000004u),"c"(0u));
        /* trim leading spaces */
        while (*brand == ' ') brand++;
        /* trim trailing spaces */
        int bi = 0; while (brand[bi]) bi++;
        while (bi > 0 && brand[bi-1] == ' ') brand[--bi] = '\0';
    }

    /* ── CPU feature bits (CPUID leaf 1) ── */
    uint32_t feat_edx = 0, feat_ecx = 0;
    if (max_basic >= 1u) {
        __asm__ volatile("cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(feat_ecx), "=d"(feat_edx)
            : "a"(1u), "c"(0u));
    }

    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    vga_print("  [ CPU ]\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("    Vendor  : "); vga_print(vendor); vga_print("\n");
    vga_print("    Model   : ");
    vga_print((max_ext >= 0x80000004u) ? brand : "(n/a)");
    vga_print("\n");
    vga_print("    Features:");
    if (feat_edx & (1u<< 0)) vga_print(" FPU");
    if (feat_edx & (1u<<23)) vga_print(" MMX");
    if (feat_edx & (1u<<25)) vga_print(" SSE");
    if (feat_edx & (1u<<26)) vga_print(" SSE2");
    if (feat_ecx & (1u<< 0)) vga_print(" SSE3");
    if (feat_ecx & (1u<< 9)) vga_print(" SSSE3");
    if (feat_ecx & (1u<<19)) vga_print(" SSE4.1");
    if (feat_ecx & (1u<<20)) vga_print(" SSE4.2");
    if (feat_ecx & (1u<<28)) vga_print(" AVX");
    vga_print("\n");

    /* ── Memory ── */
    uint32_t total_mb = (uint32_t)((pmm_get_total_frames() * 4u) / 1024u);
    uint32_t free_mb  = (uint32_t)((pmm_get_free_frames()  * 4u) / 1024u);

    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    vga_print("  [ MEMORY ]\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("    Total   : "); print_uint(total_mb); vga_print(" MB\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("    Free    : "); print_uint(free_mb);  vga_print(" MB\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* ── Storage ── */
    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    vga_print("  [ STORAGE ]\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    if (ata_master.present) {
        vga_print("    Model   : ");
        vga_print(ata_master.model[0] ? ata_master.model : "(unknown)");
        vga_print("\n");
        vga_print("    Sectors : "); print_uint(ata_master.sectors); vga_print("\n");
        vga_print("    Size    : "); print_uint(ata_master.sectors / 2048u); vga_print(" MB\n");
    } else {
        vga_print("    No ATA drive detected.\n");
    }
    vga_print("\n");
}

/* 3-column help row: command (green) | arguments (cyan) | description (white) */
#define HCMD(name, args, desc) \
    do { vga_set_color(VGA_COLOR_LIGHT_GREEN,  VGA_COLOR_BLACK); vga_print("  " name); \
         vga_set_color(VGA_COLOR_LIGHT_CYAN,   VGA_COLOR_BLACK); vga_print(args);      \
         vga_set_color(VGA_COLOR_WHITE,        VGA_COLOR_BLACK); vga_print(desc "\n"); } while(0)

/* Section header */
#define HSEC(title) \
    do { vga_print("\n"); \
         vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK); vga_print("  " title "\n"); \
         vga_set_color(VGA_COLOR_DARK_GREY,   VGA_COLOR_BLACK); \
         vga_print("  COMMAND    ARGUMENTS              DESCRIPTION\n"); \
         vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK); } while(0)

static void cmd_help(void)
{
    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  IRONKERNEL  \xe2\x80\x94  COMMAND REFERENCE\n");

    /* ── System ── */
    HSEC("[ SYSTEM ]");
    HCMD("info     ", "                       ", "CPU, RAM, disk, and hardware summary");
    HCMD("fetch    ", "                       ", "system info and colour palette");
    HCMD("version  ", "                       ", "kernel version string");
    HCMD("uptime   ", "                       ", "time since boot");
    HCMD("mem      ", "                       ", "physical + heap memory usage");
    HCMD("memstat  ", "                       ", "detailed heap block statistics");
    HCMD("memdump  ", "<addr> [count]         ", "hexdump memory  e.g. memdump 0x01000000 64");
    HCMD("leaks    ", "                       ", "dump live (unreleased) allocations");
    HCMD("ps       ", "                       ", "list running tasks");
    HCMD("dmesg    ", "                       ", "dump kernel message log");
    HCMD("beep     ", "<freq> <ms>            ", "play tone  e.g. beep 440 500");
    HCMD("play     ", "<file.wav>             ", "play WAV file via AC97  e.g. play music.wav");
    HCMD("echo     ", "<text...>              ", "print text to screen");
    HCMD("clear    ", "                       ", "clear the screen");
    HCMD("gfx      ", "                       ", "true-color VBE demo (800x600 32bpp)");
    HCMD("gui      ", "                       ", "launch window manager");
    HCMD("shutdown ", "                       ", "power off  (alias: poweroff)");
    HCMD("reboot   ", "                       ", "warm reboot");
    HCMD("halt     ", "                       ", "halt CPU");

    /* ── Hardware ── */
    HSEC("[ HARDWARE ]");
    HCMD("disk     ", "                       ", "ATA disk info");
    HCMD("lspci    ", "                       ", "list PCI devices");
    HCMD("diskread ", "<sector>               ", "hex-dump a disk sector");
    HCMD("fsinfo   ", "                       ", "FAT32 volume geometry");

    /* ── Network ── */
    HSEC("[ NETWORK ]");
    HCMD("netinfo  ", "                       ", "show IP, gateway, DNS, MAC");
    HCMD("dhcp     ", "                       ", "request IP from DHCP server");
    HCMD("nslookup ", "<host|ip>              ", "DNS forward or reverse lookup");
    HCMD("ping     ", "<host|ip>              ", "ICMP echo request");
    HCMD("wget     ", "<host[/path]>          ", "HTTP GET  e.g. wget neverssl.com/");
    HCMD("browser  ", "[http://]host[/path]   ", "text-mode HTML browser (GUI only)");
    HCMD("udpsend  ", "<ip> <port> <msg>      ", "send a UDP datagram");
    HCMD("netsend  ", "                       ", "send raw ARP broadcast (NIC test)");

    /* ── Files ── */
    HSEC("[ FILES ]");
    HCMD("ls       ", "[dir]                  ", "list directory contents");
    HCMD("pwd      ", "                       ", "print working directory");
    HCMD("cd       ", "<dir|..|/|->           ", "change directory");
    HCMD("mkdir    ", "[-p] <dir...>          ", "create directory/directories");
    HCMD("touch    ", "<file>                 ", "create empty file");
    HCMD("cat      ", "<file...>              ", "print file contents");
    HCMD("write    ", "<file> <content>       ", "write text to a file");
    HCMD("cp       ", "<src> <dst|dir/>       ", "copy a file");
    HCMD("mv       ", "<src> <dst|dir/>       ", "move or rename a file");
    HCMD("rm       ", "<file...>              ", "delete file(s)");
    HCMD("edit     ", "<file>                 ", "full-screen editor  Ctrl+S save  Ctrl+X exit");
    HCMD("exec     ", "<file.elf>             ", "load and run an ELF64 binary");

    vga_print("\n");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_print("  <required>  [optional]  Extra spaces between arguments are ignored.\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_echo(const char* args)
{
    vga_print("\n  ");
    if (*args) {
        vga_print(args);
    }
    vga_print("\n");
}

static void cmd_disk(void)
{
    if (!ata_master.present) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  No disk detected.\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  Disk\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("    Model   : ");
    vga_print(ata_master.model[0] ? ata_master.model : "(unknown)");
    vga_print("\n    Sectors : ");
    char buf[12]; buf[11] = '\0'; int i = 10;
    uint32_t s = ata_master.sectors;
    if (!s) { vga_print("0"); }
    else { while (s&&i>=0){buf[i--]='0'+(s%10);s/=10;} vga_print(&buf[i+1]); }
    vga_print("\n");
}

static void cmd_diskread(const char* args)
{
    /* Parse LBA argument. */
    uint32_t lba = 0;
    while (*args >= '0' && *args <= '9') {
        lba = lba * 10 + (uint32_t)(*args - '0');
        args++;
    }
    if (ata_master.present && lba >= ata_master.sectors) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  Sector out of range. Max: ");
        char mb[12]; mb[11] = '\0'; int mi = 10;
        uint32_t ms = ata_master.sectors - 1;
        if (!ms) { vga_print("0"); }
        else { while (ms&&mi>=0){mb[mi--]='0'+(ms%10);ms/=10;} vga_print(&mb[mi+1]); }
        vga_print("\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    uint8_t buf[512];
    if (ata_read_sector(lba, buf) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  Read failed.\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    vga_print("\n  Sector ");
    char nbuf[12]; nbuf[11]='\0'; int j=10;
    uint32_t n=lba;
    if(!n){vga_print("0");}
    else{while(n&&j>=0){nbuf[j--]='0'+(n%10);n/=10;}vga_print(&nbuf[j+1]);}
    vga_print(" (first 64 bytes):\n  ");
    for (int i = 0; i < 64; i++) {
        uint8_t byte = buf[i];
        char hex[3];
        hex[2] = '\0';
        hex[0] = "0123456789ABCDEF"[byte >> 4];
        hex[1] = "0123456789ABCDEF"[byte & 0xF];
        vga_print(hex);
        vga_print(" ");
        if ((i+1) % 16 == 0) vga_print("\n  ");
    }
    vga_print("\n");
}

static void cmd_fetch(void)
{
    uint32_t total_kb = pmm_get_total_frames() * 4;
    uint32_t free_kb  = pmm_get_free_frames()  * 4;
    uint32_t ticks    = pit_get_ticks();
    uint32_t uptime_s = ticks / 100;

    vga_print("\n");

    /* ── ASCII LOGO ─────────────────────────────────────────────── */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  ___ ____  ___  _  _ _  _____ ___ _  _ ___ _    \n");
    vga_set_color(VGA_COLOR_CYAN, VGA_COLOR_BLACK);
    vga_print(" |_ _||  _ \\/ _ \\| \\| |/ /| __|| _ \\ \\| || __|| |  \n");
    vga_print("  | | | |_) | (_) | .`' <| _| |   / .`|| _| || |_ \n");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_print(" |___|____/ \\___/|_|\\_|\\_\\|___||_|\\_|_|\\_||___||___| \n");
    vga_print("\n");

    /* ── SYSTEM INFO ────────────────────────────────────────────── */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("   OS      ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print(SHELL_NAME " v" SHELL_VERSION "\n");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("   ARCH    ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print(SHELL_ARCH "\n");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("   YEAR    ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print(SHELL_YEAR "\n");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("   SHELL   ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("iksh\n");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("   CPU     ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("x86_64 long mode\n");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("   MEMORY  ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    print_uint(free_kb);
    vga_print(" KB free / ");
    print_uint(total_kb);
    vga_print(" KB total\n");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("   UPTIME  ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    print_uint(uptime_s / 60);
    vga_print("m ");
    print_uint(uptime_s % 60);
    vga_print("s\n");

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("   TICKS   ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    print_uint(ticks);
    vga_print(" @ 100Hz\n");

    /* ── COLOUR PALETTE ─────────────────────────────────────────── */
    vga_print("\n   ");
    vga_color_t colors[] = {
        VGA_COLOR_BLACK,        VGA_COLOR_BLUE,
        VGA_COLOR_GREEN,        VGA_COLOR_CYAN,
        VGA_COLOR_RED,          VGA_COLOR_MAGENTA,
        VGA_COLOR_BROWN,        VGA_COLOR_LIGHT_GREY,
        VGA_COLOR_DARK_GREY,    VGA_COLOR_LIGHT_BLUE,
        VGA_COLOR_LIGHT_GREEN,  VGA_COLOR_LIGHT_CYAN,
        VGA_COLOR_LIGHT_RED,    VGA_COLOR_LIGHT_MAGENTA,
        VGA_COLOR_LIGHT_BROWN,  VGA_COLOR_WHITE
    };
    for (int i = 0; i < 16; i++) {
        vga_set_color(colors[i], colors[i]);
        vga_print("  ");
        /* Two spaces with bg=fg = solid color block. */
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("\n");
}

/* ── FAT12 SHELL COMMANDS ───────────────────────────────────────── */

static uint8_t cat_file_buf[4096];
/* File read buffer for cmd_cat. Declared at file scope to avoid
   stack allocation of 4KB inside a kernel function. */

static void cmd_fsinfo(void)
{
    fat32_print_info();
}

/* ── cmd_lspci ───────────────────────────────────────────────────── */
static void cmd_lspci(void)
{
    static const struct { uint16_t v; const char *name; } vendors[] = {
        { 0x8086, "Intel"      },
        { 0x1234, "QEMU/Bochs" },
        { 0x1AF4, "VirtIO"     },
        { 0x10EC, "Realtek"    },
        { 0x10DE, "NVIDIA"     },
        { 0x1002, "AMD"        },
    };
    static const struct { uint8_t cls, sub; const char *name; } classes[] = {
        { 0x00, 0x01, "VGA (old)"         },
        { 0x01, 0x01, "IDE Controller"    },
        { 0x01, 0x06, "SATA Controller"   },
        { 0x02, 0x00, "Ethernet"          },
        { 0x03, 0x00, "VGA Display"       },
        { 0x06, 0x00, "Host Bridge"       },
        { 0x06, 0x01, "ISA Bridge"        },
        { 0x06, 0x04, "PCI-PCI Bridge"    },
        { 0x06, 0x80, "Other Bridge"      },
        { 0x07, 0x00, "Serial"            },
        { 0x0C, 0x03, "USB Controller"    },
    };

    int n = pci_count();
    vga_print("\n");

    if (n == 0) {
        vga_print("  no PCI devices found\n");
        return;
    }

    for (int i = 0; i < n; i++) {
        const pci_device_t *d = pci_get(i);

        /* [bb:dd.f] */
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        char loc[9];
        static const char *hex = "0123456789ABCDEF";
        loc[0] = '[';
        loc[1] = hex[(d->bus >> 4) & 0xF];
        loc[2] = hex[d->bus & 0xF];
        loc[3] = ':';
        loc[4] = hex[(d->dev >> 1) & 0xF];
        loc[5] = hex[((d->dev & 1) << 3) | (d->fn & 0x7)];
        loc[6] = ']';
        loc[7] = ' ';
        loc[8] = '\0';
        vga_print(loc);

        /* VVVV:DDDD */
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        char ids[10];
        ids[0] = hex[(d->vendor_id >> 12) & 0xF];
        ids[1] = hex[(d->vendor_id >>  8) & 0xF];
        ids[2] = hex[(d->vendor_id >>  4) & 0xF];
        ids[3] = hex[ d->vendor_id        & 0xF];
        ids[4] = ':';
        ids[5] = hex[(d->device_id >> 12) & 0xF];
        ids[6] = hex[(d->device_id >>  8) & 0xF];
        ids[7] = hex[(d->device_id >>  4) & 0xF];
        ids[8] = hex[ d->device_id        & 0xF];
        ids[9] = '\0';
        vga_print(ids);
        vga_print("  ");

        /* class name */
        const char *cname = "Unknown";
        for (int c = 0; c < (int)(sizeof(classes)/sizeof(classes[0])); c++) {
            if (classes[c].cls == d->class_code && classes[c].sub == d->subclass) {
                cname = classes[c].name;
                break;
            }
        }
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_print(cname);
        vga_print("  ");

        /* vendor name */
        const char *vname = "";
        for (int v = 0; v < (int)(sizeof(vendors)/sizeof(vendors[0])); v++) {
            if (vendors[v].v == d->vendor_id) { vname = vendors[v].name; break; }
        }
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_print(vname);
        vga_print("\n");
    }

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_ls(const char* args)
{
    while (*args == ' ') args++;
    vga_print("\n");

    if (!*args) {
        fat32_list_dir(fat32_cwd_cluster);
        return;
    }

    /* ls [path] — navigate to path, list, return */
    static fat32_cwd_state_t ls_saved;
    fat32_save_cwd(&ls_saved);
    if (fat32_chdir(args) == 0) {
        fat32_list_dir(fat32_cwd_cluster);
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("  ls: not found: "); vga_print(args); vga_print("\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
    fat32_restore_cwd(&ls_saved);
}

/* Static 8.3 name buffers — kept at file scope to avoid stack growth. */
static char cat_name[9];
static char cat_ext[4];
static char cat_path_buf[64];   /* full path token for path-aware cat */
static char cat_dir_buf[64];    /* directory component for chdir */
static char exec_name[9];
static char exec_ext[4];

/* ── parse83 ──────────────────────────────────────────────────────────
   Parse "filename.ext" from *src into space-padded 8.3 uppercase fields.
   Advances *src past the parsed token. Returns 1 if a token was found. */
static int parse83(const char **src, char name[9], char ext[4])
{
    while (**src == ' ') (*src)++;
    if (!**src) return 0;

    for (int i = 0; i < 8; i++) name[i] = ' ';  name[8] = 0;
    for (int i = 0; i < 3; i++) ext[i]  = ' ';  ext[3]  = 0;

    int i = 0;
    while (**src && **src != '.' && **src != ' ' && i < 8) {
        char c = *(*src)++;
        name[i++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    if (**src == '.') {
        (*src)++;
        i = 0;
        while (**src && **src != ' ' && i < 3) {
            char c = *(*src)++;
            ext[i++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
    }
    while (**src == ' ') (*src)++;
    return 1;
}

static void cmd_cat(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: cat FILE [FILE2...]\n"); return; }

    int found_any = 0;
    vga_print("\n");
    /* Print each file in sequence — supports paths like LOGS/SESSION.LOG */
    while (*args) {
        /* Extract the full path token (stop at space) */
        int plen = 0;
        while (args[plen] && args[plen] != ' ' && plen < 63) plen++;
        for (int i = 0; i < plen; i++) cat_path_buf[i] = args[i];
        cat_path_buf[plen] = 0;
        args += plen;
        while (*args == ' ') args++;

        /* Find last '/' to split into directory + filename */
        int last_slash = -1;
        for (int i = 0; i < plen; i++)
            if (cat_path_buf[i] == '/') last_slash = i;

        fat32_cwd_state_t saved_cwd;
        const char *file_part = cat_path_buf;
        int did_chdir = 0;

        if (last_slash >= 0) {
            fat32_save_cwd(&saved_cwd);
            did_chdir = 1;
            /* Build dir path: everything before the last slash */
            int dlen = last_slash;
            if (dlen == 0) { cat_dir_buf[0] = '/'; cat_dir_buf[1] = 0; }
            else { for (int i = 0; i < dlen; i++) cat_dir_buf[i] = cat_path_buf[i]; cat_dir_buf[dlen] = 0; }
            if (fat32_chdir(cat_dir_buf) != 0) {
                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                vga_print("cat: not found: "); vga_print(cat_path_buf); vga_print("\n");
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                fat32_restore_cwd(&saved_cwd);
                continue;
            }
            file_part = cat_path_buf + last_slash + 1;
        }

        const char *fp = file_part;
        int parsed = parse83(&fp, cat_name, cat_ext);
        uint32_t bytes = 0;
        int ret = parsed ? fat32_read_file(cat_name, cat_ext, cat_file_buf, 4095, &bytes) : -1;

        if (did_chdir) fat32_restore_cwd(&saved_cwd);

        if (!parsed || ret != 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_print("cat: not found: "); vga_print(cat_path_buf); vga_print("\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            continue;
        }
        found_any = 1;
        cat_file_buf[bytes] = '\0';
        for (uint32_t b = 0; b < bytes; b++) {
            uint8_t c = cat_file_buf[b];
            if (c == '\r') continue;
            char s[2] = {(char)c, 0};
            vga_print(s);
        }
    }
    if (found_any) vga_print("\n");
}

static void cmd_exec(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: exec FILE.ELF [ARG]\n"); return; }

    if (!parse83(&args, exec_name, exec_ext)) return;

    /* Store optional argument (e.g. filename for edit.elf) on current task */
    task_t *cur_task = sched_get_tasks() + sched_current_id();
    int ak = 0;
    while (*args && ak < 31) cur_task->elf_arg[ak++] = *args++;
    cur_task->elf_arg[ak] = 0;

    /* Log exec to dmesg */
    {
        static char exec_log[32];
        static const char px[] = "[EXEC] ";
        int li = 0;
        for (; px[li]; li++) exec_log[li] = px[li];
        for (int j = 0; j < 8 && exec_name[j] != ' '; j++) exec_log[li++] = exec_name[j];
        exec_log[li++] = '.';
        for (int j = 0; j < 3 && exec_ext[j] != ' '; j++) exec_log[li++] = exec_ext[j];
        exec_log[li] = '\0';
        klog(LOG_INFO, exec_log);
    }
    vga_print("\n  [EXEC] ");
    for (int j = 0; j < 8 && exec_name[j] != ' '; j++) {
        char s[2] = {exec_name[j], 0}; vga_print(s);
    }
    vga_print(".");
    for (int j = 0; j < 3 && exec_ext[j] != ' '; j++) {
        char s[2] = {exec_ext[j], 0}; vga_print(s);
    }
    vga_print("\n\n");
    elf_exec(exec_name, exec_ext);
}

static void cmd_ps(void)
{
    task_t* tasks = sched_get_tasks();
    int     count = sched_get_count();
    int     cur   = sched_current_id();

    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  ID  STATE    TICKS     NAME              WIN\n");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    vga_print("  --  -------  --------  ----------------  -------------------\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    const char* states[] = {"READY  ", "RUNNING", "DEAD   ", "ZOMBIE "};

    for (int i = 0; i < count; i++) {
        /* ID */
        vga_set_color(i == cur ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_WHITE,
                      VGA_COLOR_BLACK);
        vga_print("  ");
        vga_putchar('0' + (char)tasks[i].id);
        vga_print("   ");

        /* STATE */
        int s = (int)tasks[i].state;
        if (s < 0 || s > 3) s = 2;
        vga_print(states[s]);
        vga_print("  ");

        /* TICKS */
        print_uint(tasks[i].ticks);
        vga_print("       ");

        /* NAME */
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_print(tasks[i].name);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        /* WIN — show window title if task has a bound window */
        int wid = tasks[i].win_id;
        if (wid >= 0 && wid < WM_MAX_WIN && wm_wins[wid].alive) {
            /* pad name to column */
            int nlen = 0;
            while (tasks[i].name[nlen]) nlen++;
            for (int p = nlen; p < 18; p++) vga_putchar(' ');
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            vga_putchar('[');
            vga_print(wm_wins[wid].title);
            vga_putchar(']');
            if (wid == wm_focused) {
                vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                vga_print(" *");
            }
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }

        vga_print("\n");
    }
    vga_print("\n");
}


/* ── EDITOR STATE ───────────────────────────────────────────────────
   Simple full-screen text editor. Ctrl+S saves, Ctrl+X exits.
   Buffer lives at file scope to avoid large stack allocations. */
#define EDIT_BUF_SIZE  4096
#define EDIT_COLS      78
#define EDIT_ROWS      23   /* VGA has 25 rows: 1 title bar + 23 edit + 1 status */
static char  edit_buf[EDIT_BUF_SIZE];
static char  edit_fname[9];
static char  edit_fext[4];

/* ── STATIC BUFFERS for new commands (file scope avoids stack) ───── */
static char mkdir_name[9];
static char write_name[9];
static char write_ext[4];
static char rm_name[9];
static char rm_ext[4];
static uint8_t write_buf[4096];

/* ── editor_redraw ──────────────────────────────────────────────────
   Full redraw of the editor screen. Layout:
     Row  0 : title bar  (cyan bg) — filename, always exactly 80 cols
     Rows 1-23 : text content
     Row 24 : status bar (grey bg) — written directly so cursor stays
              at the end of the content, where the user is typing. */
static void editor_redraw(uint32_t len)
{
    vga_init();

    /* ── Title bar: build exact 80-char string, write without moving cursor ── */
    char title[81];
    int ti = 0;
    const char *hdr = "  IK EDIT  |  ";   /* 14 chars */
    for (; hdr[ti]; ti++) title[ti] = hdr[ti];
    for (int i = 0; i < 8 && edit_fname[i] != ' '; i++) title[ti++] = edit_fname[i];
    title[ti++] = '.';
    for (int i = 0; i < 3 && edit_fext[i] != ' '; i++) title[ti++] = edit_fext[i];
    while (ti < 80) title[ti++] = ' ';
    title[80] = 0;
    vga_write_at(0, 0, title, VGA_COLOR_BLACK, VGA_COLOR_LIGHT_CYAN);

    /* ── Content: position cursor at row 1, col 0, then print buffer ── */
    vga_goto(1, 0);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (uint32_t b = 0; b < len; b++) {
        char s[2] = {edit_buf[b], 0};
        vga_print(s);
    }
    /* Cursor is now at the end of the content — correct typing position. */

    /* ── Status bar: write directly to row 24 without moving cursor ── */
    vga_write_at(24, 0, "  ^S Save  |  ^X Exit",
                 VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREY);
}

/* ── cmd_edit ───────────────────────────────────────────────────────
   Minimal full-screen text editor, inspired by nano.
   Title bar shows filename. Status bar shows Ctrl keybinds.
   Ctrl+S  saves file to FAT12 disk and stays in editor.
   Ctrl+X  exits editor without saving.
   Backspace deletes last character. Enter inserts newline.
   The editor works on a flat character buffer — no line tracking. */
static void cmd_edit(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: edit FILE.EXT\n"); return; }

    /* Parse filename into 8.3 format. */
    int i = 0;
    const char* p = args;
    while (*p && *p != '.' && *p != ' ' && i < 8) {
        char c = *p++;
        edit_fname[i++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    for (; i < 8; i++) edit_fname[i] = ' ';
    edit_fname[8] = 0;
    int j = 0;
    for (j = 0; j < 3; j++) edit_fext[j] = ' ';
    edit_fext[3] = 0;
    if (*p == '.') {
        p++; j = 0;
        while (*p && *p != ' ' && j < 3) {
            char c = *p++;
            edit_fext[j++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
    }

    /* Try to load existing file contents into buffer. */
    uint32_t loaded = 0;
    for (i = 0; i < EDIT_BUF_SIZE; i++) edit_buf[i] = 0;
    fat32_read_file(edit_fname, edit_fext,
                    (uint8_t*)edit_buf, EDIT_BUF_SIZE - 1, &loaded);
    uint32_t len = loaded;

    editor_redraw(len);

    /* Flush any leftover keystrokes from the command line. */
    while (keyboard_haschar()) keyboard_getchar();

    /* ── EDIT LOOP ─────────────────────────────────────────────────── */
    for (;;) {
        while (!keyboard_haschar())
            __asm__ volatile("hlt");

        char c = keyboard_getchar();

        if (c == 0x13) {    /* Ctrl+S — save file to FAT12 disk. */
            fat32_write_file(edit_fname, edit_fext,
                             (uint8_t*)edit_buf, len);
            vga_write_at(24, 0, "  SAVED  —  Ctrl+S to save again  |  Ctrl+X to exit",
                         VGA_COLOR_BLACK, VGA_COLOR_LIGHT_GREEN);
            continue;
        }
        if (c == 0x18) {    /* Ctrl+X — exit editor. */
            vga_init();
            scroll_reset();
            break;
        }
        if (c > 0 && c < 0x20 && c != '\n' && c != '\b') continue; /* other ctrl */

        if (c == '\b') {
            /* Backspace — remove last character from buffer. */
            if (len > 0) { len--; edit_buf[len] = 0; editor_redraw(len); }
            continue;
        }

        if (len < EDIT_BUF_SIZE - 1) {
            edit_buf[len++] = c;
            edit_buf[len]   = 0;
            /* Only full redraw on newline; otherwise append char directly. */
            if (c == '\n') {
                editor_redraw(len);
            } else {
                char s[2] = {c, 0};
                vga_print(s);
            }
        }
    }
}

/* ── mkdir_one ──────────────────────────────────────────────────────
   Create a single directory component in the current directory.
   Returns 0 on success, -1 if it already exists, -2 on other error. */
static int mkdir_one(const char* name)
{
    int i = 0;
    while (*name && *name != ' ' && *name != '/' && i < 8) {
        char c = *name++;
        mkdir_name[i++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    mkdir_name[i] = 0;
    if (i == 0) return 0;  /* empty component */
    return fat32_mkdir(mkdir_name);
}

/* ── cmd_mkdir ──────────────────────────────────────────────────────
   Supports:
     mkdir DIR               — create one directory
     mkdir DIR1 DIR2 DIR3    — create multiple directories
     mkdir -p DIR/SUBDIR     — create nested path (mkdir -p style) */
static void cmd_mkdir(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) {
        vga_print("\n  Usage: mkdir [-p] DIR [DIR2 ...]\n");
        return;
    }

    /* Check for -p flag */
    int parents = 0;
    if (args[0] == '-' && args[1] == 'p' && (args[2] == ' ' || args[2] == 0)) {
        parents = 1;
        args += 2;
        while (*args == ' ') args++;
    }

    int any_created = 0;
    int any_error   = 0;

    /* Process each space-separated argument */
    while (*args) {
        /* Extract next token (path) */
        const char* tok = args;
        while (*args && *args != ' ') args++;
        /* null-terminate by copying into a local buf */
        static char path_buf[64];
        int pi = 0;
        const char* tp = tok;
        while (tp < args && pi < 63) path_buf[pi++] = *tp++;
        path_buf[pi] = 0;
        while (*args == ' ') args++;

        if (parents) {
            /* -p mode: walk each component, creating as needed */
            const char* p = path_buf;
            /* If absolute path, start from root */
            if (*p == '/') {
                fat32_chdir("/");
                p++;
            }
            while (*p) {
                /* Extract component */
                static char comp[9];
                int ci = 0;
                while (*p && *p != '/' && ci < 8) comp[ci++] = *p++;
                comp[ci] = 0;
                if (*p == '/') p++;
                if (ci == 0) continue;
                /* Try to create; ignore "already exists" (-1) */
                int r = mkdir_one(comp);
                if (r == 0) {
                    any_created = 1;
                    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                    vga_print("\n  created: ");
                    vga_print(mkdir_name);
                }
                /* Enter the directory to continue creating nested dirs */
                if (fat32_chdir(comp) != 0) {
                    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                    vga_print("\n  mkdir -p: cannot enter: ");
                    vga_print(comp);
                    any_error = 1;
                    break;
                }
            }
            /* Return to original directory */
            /* (chdir already happened during creation — restore via cd abs path) */
        } else {
            /* Normal mode: create directory in current location */
            const char* p = path_buf;
            if (mkdir_one(p) == 0) {
                any_created = 1;
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                vga_print("\n  created: ");
                vga_print(mkdir_name);
            } else {
                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                vga_print("\n  mkdir: cannot create '");
                vga_print(path_buf);
                vga_print("' (exists or dir full)");
                any_error = 1;
            }
        }
    }

    if (!any_created && !any_error) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  mkdir: nothing created");
    }
    vga_print("\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ── cmd_cd ─────────────────────────────────────────────────────────
   Changes the current working directory.
   Supports paths like cd dir/subdir, cd .., cd /, cd -.
   cd with no args → root (Unix convention). */
static void cmd_cd(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) {
        /* cd with no args → root */
        fat32_chdir("/");
        return;
    }
    if (fat32_chdir(args) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  cd: not found: ");
        vga_print(args);
        vga_print("\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

/* ── cmd_write ──────────────────────────────────────────────────────
   write FILE.EXT content — create/overwrite file with inline content. */
static void cmd_write(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: write FILE.EXT content\n"); return; }

    if (!parse83(&args, write_name, write_ext)) return;

    /* Copy content (rest of line) into write_buf. */
    uint32_t len = 0;
    while (*args && len < 4095)
        write_buf[len++] = (uint8_t)*args++;
    write_buf[len] = 0;

    if (fat32_write_file(write_name, write_ext, write_buf, len) == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_print("\n  written: "); vga_print(write_name);
        vga_print("."); vga_print(write_ext); vga_print("\n");
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  write failed\n");
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ── cmd_rm ─────────────────────────────────────────────────────────
   rm FILE [FILE2 ...] — delete one or more files. */
static void cmd_rm(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: rm FILE [FILE2...]\n"); return; }

    vga_print("\n");
    while (*args) {
        if (!parse83(&args, rm_name, rm_ext)) break;
        if (fat32_delete(rm_name, rm_ext) == 0) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            vga_print("  rm: deleted ");
            vga_print(rm_name); vga_print("."); vga_print(rm_ext);
            vga_print("\n");
        } else {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_print("  rm: not found: ");
            vga_print(rm_name); vga_print("."); vga_print(rm_ext);
            vga_print("\n");
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

/* Static name/token buffers for cp/mv.
   Data buffer is kmalloc'd at call time to handle files of any size. */
static char cp_src_name[9], cp_src_ext[4];
static char cp_dst_name[9], cp_dst_ext[4];
static char cp_token[64];

/* print83 — print an 8.3 name or ext field, stopping at the first space. */
static void print83(const char *s, int max)
{
    for (int i = 0; i < max && s[i] && s[i] != ' '; i++) {
        char b[2] = {s[i], 0}; vga_print(b);
    }
}

/* get_token — extract the next space-delimited token from *src. */
static void get_token(const char **src, char *out, int max)
{
    while (**src == ' ') (*src)++;
    int i = 0;
    while (**src && **src != ' ' && i < max - 1)
        out[i++] = *(*src)++;
    out[i] = 0;
    while (**src == ' ') (*src)++;
}

/* cp_resolve_dest — interpret cp_token as a destination path, navigate
   CWD to the target directory, and fill cp_dst_name / cp_dst_ext.
   The caller must save/restore CWD around this call.
   Returns 0 on success, -1 on navigation or parse error. */
static int cp_resolve_dest(const char *src_name, const char *src_ext)
{
    int tlen = 0;
    while (cp_token[tlen]) tlen++;

    int last_slash = -1;
    for (int i = 0; i < tlen; i++) if (cp_token[i] == '/') last_slash = i;

    if (last_slash >= 0) {
        /* Split at last '/': path part | filename part */
        cp_token[last_slash] = 0;
        const char *fname = cp_token + last_slash + 1;
        /* Navigate to path part: empty means root (/), otherwise chdir */
        if (last_slash == 0) {
            fat32_chdir("/");               /* absolute path: go to root */
        } else if (fat32_chdir(cp_token) != 0) {
            return -1;
        }
        if (*fname) {
            /* Check if the final component is itself a directory */
            int fname_has_dot = 0;
            for (int i = 0; fname[i]; i++) if (fname[i] == '.') { fname_has_dot = 1; break; }
            if (!fname_has_dot && fat32_chdir(fname) == 0) {
                /* It's a directory — keep source name */
                for (int i = 0; i < 9; i++) cp_dst_name[i] = src_name[i];
                for (int i = 0; i < 4; i++) cp_dst_ext[i]  = src_ext[i];
            } else {
                const char *p = fname;
                if (!parse83(&p, cp_dst_name, cp_dst_ext)) return -1;
            }
        } else {
            /* Token ended with '/' — keep source name */
            for (int i = 0; i < 9; i++) cp_dst_name[i] = src_name[i];
            for (int i = 0; i < 4; i++) cp_dst_ext[i]  = src_ext[i];
        }
    } else {
        /* No slash: token with a dot is a filename; without a dot try as dir */
        int has_dot = 0;
        for (int i = 0; i < tlen; i++) if (cp_token[i] == '.') { has_dot = 1; break; }
        if (has_dot) {
            const char *p = (const char*)cp_token;
            if (!parse83(&p, cp_dst_name, cp_dst_ext)) return -1;
        } else if (fat32_chdir(cp_token) == 0) {
            /* Successfully entered a subdirectory — use source name */
            for (int i = 0; i < 9; i++) cp_dst_name[i] = src_name[i];
            for (int i = 0; i < 4; i++) cp_dst_ext[i]  = src_ext[i];
        } else {
            /* Not a directory — treat as filename without extension */
            const char *p = (const char*)cp_token;
            if (!parse83(&p, cp_dst_name, cp_dst_ext)) return -1;
        }
    }
    return 0;
}

/* CP_BUF_MAX — match the ELF loader's file size limit so ELF binaries
   copy intact.  kmalloc'd at call time; freed before returning. */
#define CP_BUF_MAX (128 * 1024)

/* ── cmd_cp ─────────────────────────────────────────────────────────
   cp SRC.EXT [DIR/]DST.EXT — copy file, optionally into a directory. */
static void cmd_cp(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: cp SRC.EXT [DIR/]DST.EXT\n"); return; }

    if (!parse83(&args, cp_src_name, cp_src_ext)) {
        vga_print("\n  cp: bad source name\n"); return;
    }
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: cp SRC.EXT [DIR/]DST.EXT\n"); return; }

    uint8_t *buf = (uint8_t*)kmalloc(CP_BUF_MAX);
    if (!buf) { vga_print("\n  cp: out of memory\n"); return; }

    /* Read source before any CWD navigation */
    uint32_t bytes = 0;
    if (fat32_read_file(cp_src_name, cp_src_ext, buf, CP_BUF_MAX, &bytes) != 0) {
        kfree(buf);
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  cp: source not found\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    get_token(&args, cp_token, sizeof(cp_token));

    fat32_cwd_state_t saved;
    fat32_save_cwd(&saved);

    if (cp_resolve_dest(cp_src_name, cp_src_ext) != 0) {
        fat32_restore_cwd(&saved);
        kfree(buf);
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  cp: destination not found\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    if (fat32_write_file(cp_dst_name, cp_dst_ext, buf, bytes) == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_print("\n  cp: ");
        print83(cp_src_name, 8); vga_print("."); print83(cp_src_ext, 3);
        vga_print(" -> ");
        print83(cp_dst_name, 8); vga_print("."); print83(cp_dst_ext, 3);
        vga_print("\n");
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  cp: write failed (disk full?)\n");
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    fat32_restore_cwd(&saved);
    kfree(buf);
}

/* ── cmd_mv ─────────────────────────────────────────────────────────
   mv SRC.EXT [DIR/]DST.EXT — rename in same dir or move to another dir. */
static void cmd_mv(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: mv SRC.EXT [DIR/]DST.EXT\n"); return; }

    if (!parse83(&args, cp_src_name, cp_src_ext)) {
        vga_print("\n  mv: bad source name\n"); return;
    }
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: mv SRC.EXT [DIR/]DST.EXT\n"); return; }

    get_token(&args, cp_token, sizeof(cp_token));

    /* Detect if destination involves a directory */
    int cross_dir = 0;
    for (int i = 0; cp_token[i]; i++) if (cp_token[i] == '/') { cross_dir = 1; break; }

    if (!cross_dir) {
        /* No slash: check if the token (no dot) is a subdirectory */
        int has_dot = 0;
        for (int i = 0; cp_token[i]; i++) if (cp_token[i] == '.') { has_dot = 1; break; }
        if (!has_dot) {
            fat32_cwd_state_t test;
            fat32_save_cwd(&test);
            if (fat32_chdir(cp_token) == 0) { cross_dir = 1; }
            fat32_restore_cwd(&test);
        }
    }

    if (!cross_dir) {
        /* Same-directory rename — in-place 8.3 rewrite, no data copy */
        const char *p = (const char*)cp_token;
        if (!parse83(&p, cp_dst_name, cp_dst_ext)) {
            vga_print("\n  mv: bad destination\n"); return;
        }
        if (fat32_rename(cp_src_name, cp_src_ext, cp_dst_name, cp_dst_ext) == 0) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            vga_print("\n  mv: ");
            print83(cp_src_name, 8); vga_print("."); print83(cp_src_ext, 3);
            vga_print(" -> ");
            print83(cp_dst_name, 8); vga_print("."); print83(cp_dst_ext, 3);
            vga_print("\n");
        } else {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_print("\n  mv: source not found\n");
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Cross-directory move: read source → navigate → write → restore → delete */
    uint8_t *buf = (uint8_t*)kmalloc(CP_BUF_MAX);
    if (!buf) { vga_print("\n  mv: out of memory\n"); return; }

    uint32_t bytes = 0;
    if (fat32_read_file(cp_src_name, cp_src_ext, buf, CP_BUF_MAX, &bytes) != 0) {
        kfree(buf);
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  mv: source not found\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    fat32_cwd_state_t saved;
    fat32_save_cwd(&saved);

    if (cp_resolve_dest(cp_src_name, cp_src_ext) != 0) {
        fat32_restore_cwd(&saved);
        kfree(buf);
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  mv: destination not found\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    if (fat32_write_file(cp_dst_name, cp_dst_ext, buf, bytes) != 0) {
        fat32_restore_cwd(&saved);
        kfree(buf);
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  mv: write failed\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    fat32_restore_cwd(&saved);
    kfree(buf);
    fat32_delete(cp_src_name, cp_src_ext);

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("\n  mv: ");
    print83(cp_src_name, 8); vga_print("."); print83(cp_src_ext, 3);
    vga_print(" -> ");
    print83(cp_dst_name, 8); vga_print("."); print83(cp_dst_ext, 3);
    vga_print("\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ── cmd_pwd ─────────────────────────────────────────────────────── */
static void cmd_pwd(void)
{
    vga_print("\n  ");
    vga_print(fat32_cwd);
    vga_print("\n");
}

/* ── cmd_touch ───────────────────────────────────────────────────────
   touch FILE.EXT — create an empty file (or leave existing unchanged). */
static void cmd_touch(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: touch FILE.EXT\n"); return; }

    static char t_name[9], t_ext[4];
    if (!parse83(&args, t_name, t_ext)) return;

    /* Write 0 bytes — creates the entry if it doesn't exist. */
    if (fat32_write_file(t_name, t_ext, (const uint8_t*)"", 0) == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_print("\n  touch: "); vga_print(t_name); vga_print("."); vga_print(t_ext); vga_print("\n");
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("\n  touch: failed\n");
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ── print_hex_byte ──────────────────────────────────────────────── */
static void print_hex_byte(uint8_t b)
{
    char h[3];
    h[0] = "0123456789ABCDEF"[b >> 4];
    h[1] = "0123456789ABCDEF"[b & 0xF];
    h[2] = '\0';
    vga_print(h);
}

/* ── cmd_netinfo ─────────────────────────────────────────────────────
   Shows NIC status and MAC address. */


static void cmd_netinfo(void)
{
    vga_print("\n");
    if (!e1000_ready()) {
        vga_print("  [NET]  no e1000 NIC detected\n");
        return;
    }
    uint8_t mac[6];
    e1000_mac(mac);
    vga_print("  NIC    Intel 82540EM (e1000)\n");
    vga_print("  MAC    ");
    for (int i = 0; i < 6; i++) {
        char h[3];
        h[0] = "0123456789ABCDEF"[mac[i] >> 4];
        h[1] = "0123456789ABCDEF"[mac[i] & 0xF];
        h[2] = '\0';
        vga_print(h);
        if (i < 5) vga_print(":");
    }
    vga_print("\n  IP     ");
    for (int i = 0; i < 4; i++) { print_uint(g_net_ip[i]); if (i < 3) vga_print("."); }
    vga_print("\n  GW     ");
    for (int i = 0; i < 4; i++) { print_uint(g_net_gw[i]); if (i < 3) vga_print("."); }
    vga_print("\n  DNS    ");
    for (int i = 0; i < 4; i++) { print_uint(g_net_dns[i]); if (i < 3) vga_print("."); }
    vga_print("\n\n");
}

static void cmd_dhcp(void)
{
    if (!e1000_ready()) { vga_print("  No NIC.\n"); return; }
    vga_print("\n  Sending DHCP Discover...\n");
    if (dhcp_discover() == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_print("  Assigned: ");
        for (int i = 0; i < 4; i++) { print_uint(g_net_ip[i]); if (i < 3) vga_print("."); }
        vga_print("\n  Gateway:  ");
        for (int i = 0; i < 4; i++) { print_uint(g_net_gw[i]); if (i < 3) vga_print("."); }
        vga_print("\n  DNS:      ");
        for (int i = 0; i < 4; i++) { print_uint(g_net_dns[i]); if (i < 3) vga_print("."); }
        vga_print("\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    } else {
        vga_print("  DHCP timeout — configuration unchanged.\n");
    }
}

static void cmd_netsend(void)
{
    if (!e1000_ready()) return;
    uint8_t mac[6]; e1000_mac(mac);
    uint8_t frame[42];
    for (int i=0; i<6; i++) frame[i]=0xFF;
    for (int i=0; i<6; i++) frame[6+i]=mac[i];
    frame[12]=0x08; frame[13]=0x06;
    frame[14]=0x00; frame[15]=0x01; frame[16]=0x08; frame[17]=0x00;
    frame[18]=6; frame[19]=4; frame[20]=0x00; frame[21]=0x01;
    for (int i=0; i<6; i++) frame[22+i]=mac[i];
    frame[28]=10; frame[29]=0; frame[30]=2; frame[31]=15;
    for (int i=0; i<6; i++) frame[32+i]=0;
    frame[38]=10; frame[39]=0; frame[40]=2; frame[41]=2;
    if (e1000_send(frame, 42) == 0) vga_print("  [NET] ARP sent.\n");
}

static void cmd_nslookup(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("  Usage: nslookup <hostname|IP>\n"); return; }

    vga_print("\n  Server: 10.0.2.3\n\n");

    if (!is_hostname(args)) {
        /* ── Reverse lookup: IP → hostname ── */
        uint8_t ip[4];
        const char *ptr = args;
        parse_ip(&ptr, ip);

        vga_print("  Reverse lookup for: ");
        for (int i = 0; i < 4; i++) { print_uint(ip[i]); if (i < 3) vga_print("."); }
        vga_print("\n");

        static char rname[256];
        if (dns_reverse(ip, rname, sizeof(rname)) == 0) {
            vga_print("  Name: ");
            vga_print(rname);
            vga_print("\n");
        } else {
            vga_print("  DNS: no PTR record found.\n");
        }
        return;
    }

    /* ── Forward lookup: hostname → IPs ── */
    vga_print("  Querying: ");
    vga_print(args);
    vga_print("\n\n");

    uint8_t list[DNS_MAX_ADDRS][4];
    int count = dns_resolve_all(args, list, DNS_MAX_ADDRS);
    if (count <= 0) {
        vga_print("  DNS: no A record found.\n");
        return;
    }

    for (int a = 0; a < count; a++) {
        vga_print("  Address: ");
        for (int i = 0; i < 4; i++) { print_uint(list[a][i]); if (i < 3) vga_print("."); }
        vga_print("\n");
    }
    vga_print("\n  Total: "); print_uint((uint32_t)count);
    vga_print(count == 1 ? " address\n" : " addresses\n");
}

static void cmd_ping(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("  Usage: ping <host|IP>\n"); return; }
    uint8_t ip[4];
    if (resolve_host(args, ip) != 0) return;
    vga_print("\n  Pinging ");
    for (int i = 0; i < 4; i++) { print_uint(ip[i]); if (i < 3) vga_print("."); }
    vga_print("...\n");
    int rtt = icmp_ping(ip, 1);
    if (rtt >= 0) {
        vga_print("  Reply from ");
        for (int i = 0; i < 4; i++) { print_uint(ip[i]); if (i < 3) vga_print("."); }
        vga_print(": time=");
        print_uint((uint32_t)rtt);
        vga_print(" ms\n");
    } else {
        vga_print("  Request timed out.\n");
    }
}

static void cmd_udpsend(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("  usage: udpsend IP PORT MSG\n"); return; }
    uint8_t ip[4];
    const char *ptr = args;
    parse_ip(&ptr, ip);
    while (*ptr == ' ') ptr++;
    uint16_t port = (uint16_t)parse_uint(ptr);
    while (*ptr && *ptr != ' ') ptr++;
    while (*ptr == ' ') ptr++;
    udp_send(ip, 1234, port, (uint8_t*)ptr, (uint16_t)str_len(ptr));
    vga_print("  UDP Packet Sent.\n");
}

static void cmd_wget(const char* args)
{
    while (*args == ' ') args++;
    if (!*args) { vga_print("\n  Usage: wget HOST[/path]  or  wget http://HOST[:PORT]/path\n"); return; }

    const char *ptr = args;

    /* Skip "http://" or "https://" prefix */
    if (ptr[0]=='h' && ptr[1]=='t' && ptr[2]=='t' && ptr[3]=='p') {
        while (*ptr && *ptr != '/') ptr++;
        if (*ptr == '/') ptr++;
        if (*ptr == '/') ptr++;
    }

    /* Extract host into its own buffer, advance ptr past it cleanly */
    char hbuf[128];
    int  hi = 0;
    while (hi < 127 && ptr[hi] && ptr[hi] != ':' && ptr[hi] != '/') {
        hbuf[hi] = ptr[hi]; hi++;
    }
    hbuf[hi] = '\0';
    ptr += hi;   /* ptr now sits exactly at ':', '/', or '\0' */

    /* Resolve the host (DNS or dotted-decimal) */
    uint8_t ip[4];
    if (resolve_host(hbuf, ip) != 0) return;

    /* Parse optional :PORT */
    uint16_t port = 80;
    if (*ptr == ':') {
        ptr++;
        port = (uint16_t)parse_uint(ptr);
        while (*ptr >= '0' && *ptr <= '9') ptr++;
    }

    /* Remainder is the path */
    const char *path = (*ptr == '/') ? ptr : "/";

    vga_print("\n  Connecting to ");
    for (int i = 0; i < 4; i++) { print_uint(ip[i]); if (i < 3) vga_print("."); }
    vga_print(":");
    print_uint(port);
    vga_print(path);
    vga_print("\n");

    static uint8_t wget_buf[4096];
    int n = http_get(ip, port, hbuf, path, wget_buf, sizeof(wget_buf) - 1);
    if (n < 0) {
        vga_print("  Error: connection failed.\n");
        return;
    }
    wget_buf[n] = '\0';
    vga_print((const char *)wget_buf);
    vga_print("\n");
    vga_print("  Done (");
    print_uint((uint32_t)n);
    vga_print(" bytes)\n");
}

/* Convert HSV (h:0-1535, s:0-255, v:0-255) to 0x00RRGGBB.
   h spans 6 sectors of 256 steps each = full hue wheel. */
static uint32_t hsv_to_rgb(uint32_t h, uint32_t s, uint32_t v)
{
    uint32_t region = h / 256;
    uint32_t rem    = h % 256;
    uint32_t p = v * (255 - s) / 255;
    uint32_t q = v * (255 - s * rem / 255) / 255;
    uint32_t t = v * (255 - s * (255 - rem) / 255) / 255;
    switch (region) {
        case 0:  return (v << 16) | (t <<  8) | p;
        case 1:  return (q << 16) | (v <<  8) | p;
        case 2:  return (p << 16) | (v <<  8) | t;
        case 3:  return (p << 16) | (q <<  8) | v;
        case 4:  return (t << 16) | (p <<  8) | v;
        default: return (v << 16) | (p <<  8) | q;
    }
}

/* ── GFX demo helpers ─────────────────────────────────────────────── */

/* Bhaskara I sine approximation — no lookup table, no libc.
   angle: 0..255 (full circle).  Returns -127..127.
   Max error < 1.8% of true sine — smooth enough for plasma effects. */
static int32_t gfx_sin(int angle)
{
    angle &= 0xFF;
    int sign = 1;
    if (angle >= 128) { sign = -1; angle -= 128; }
    /* Bhaskara: sin(θ) ≈ 16·x·(128-x) / (5·128² - 4·x·(128-x))
       where x = angle ∈ [0,128] represents θ ∈ [0,π]. */
    int32_t n   = (int32_t)angle * (128 - angle);   /* 0..4096    */
    int32_t num = 16 * n;                            /* 0..65536   */
    int32_t den = 81920 - 4 * n;                    /* 65536..81920 */
    return (int32_t)(sign * num * 127 / den);        /* -127..127  */
}

/* Q16.16 fixed-point: compile-time float literal → integer constant */
#define GFXFP(f)    ((int32_t)((f) * 65536.0))
/* Q16.16 multiply via 64-bit intermediate */
#define GFXMUL(a,b) ((int32_t)(((int64_t)(a) * (b)) >> 16))

/* Cool blue/cyan palette — Mandelbrot */
static uint32_t gfx_cool_col(int iter, int max_iter)
{
    if (iter >= max_iter) return 0x00040810u;
    uint32_t hue = ((uint32_t)iter * 28u) % 1536u;
    uint32_t val = 140u + (uint32_t)iter * 115u / (uint32_t)max_iter;
    return hsv_to_rgb(hue, 255, val);
}

/* Fire palette (red→orange→yellow) — Burning Ship */
static uint32_t gfx_fire_col(int iter, int max_iter)
{
    if (iter >= max_iter) return 0x00000000u;
    uint32_t hue = (uint32_t)iter * 320u / (uint32_t)max_iter;
    uint32_t sat = 255u - (uint32_t)iter * 80u / (uint32_t)max_iter;
    uint32_t val = 120u + (uint32_t)iter * 135u / (uint32_t)max_iter;
    return hsv_to_rgb(hue, sat, val);
}

/* Mandelbrot set — region [-2.5, 1.0] × [-1.2, 1.2], 80 iterations */
static uint32_t gfx_mandel(int px, int py, int pw, int ph)
{
    int32_t cr = GFXFP(-2.5) + (int32_t)((int64_t)px * GFXFP(3.5) / pw);
    int32_t ci = GFXFP(-1.2) + (int32_t)((int64_t)py * GFXFP(2.4) / ph);
    int32_t zr = 0, zi = 0;
    int i;
    for (i = 0; i < 80; i++) {
        int32_t r2 = GFXMUL(zr, zr), i2 = GFXMUL(zi, zi);
        if (r2 + i2 > GFXFP(4)) break;
        zi = 2 * GFXMUL(zr, zi) + ci;
        zr = r2 - i2 + cr;
    }
    return gfx_cool_col(i, 80);
}

/* Burning Ship fractal — z = (|Re(z)| + i|Im(z)|)² + c
   Region [-2.5, 1.5] × [-2.0, 0.5] (flipped Y = "ship" orientation) */
static uint32_t gfx_burning_ship(int px, int py, int pw, int ph)
{
    int32_t cr = GFXFP(-2.5) + (int32_t)((int64_t)px * GFXFP(4.0) / pw);
    int32_t ci = GFXFP(-2.0) + (int32_t)((int64_t)py * GFXFP(2.5) / ph);
    int32_t zr = 0, zi = 0;
    int i;
    for (i = 0; i < 80; i++) {
        int32_t ar = zr < 0 ? -zr : zr;   /* |Re(z)| */
        int32_t ai = zi < 0 ? -zi : zi;   /* |Im(z)| */
        int32_t r2 = GFXMUL(ar, ar), i2 = GFXMUL(ai, ai);
        if (r2 + i2 > GFXFP(4)) break;
        zi = 2 * GFXMUL(ar, ai) + ci;
        zr = r2 - i2 + cr;
    }
    return gfx_fire_col(i, 80);
}

#undef GFXFP
#undef GFXMUL

static void cmd_gfx(void)
{
    /* ── IronKernel Graphics Showcase ─────────────────────────────
       Title bar    (y=  0.. 19): gradient label strip
       Sine plasma  (y= 20..309): 800×290 — Bhaskara sine interference
       Divider      (y=310..311): separator
       Mandelbrot   (y=312..599): 400×288 — cool rainbow palette
       Burning Ship (y=312..599): 400×288 — fire palette
       ────────────────────────────────────────────────────────────*/

    /* ── Title bar: magenta→teal gradient ────────────────────────── */
    for (int x = 0; x < 800; x++) {
        uint32_t hue = 800u + (uint32_t)x * 500u / 800u;
        for (int ty = 0; ty < 20; ty++)
            vga_pixel(x, ty, hsv_to_rgb(hue % 1536u, 200, 50));
    }
    vga_hline(0, 20, 800, 0x182030);
    vga_print_gfx(  6,  5, "IRONKERNEL", 0xAAEEFF);
    vga_print_gfx( 90,  5, "\xe2\x80\x94  GRAPHICS SHOWCASE  \xe2\x80\x94  800\xc3\x97""600\xc3\x97""32bpp", 0xBBBBBB);

    /* ── Sine Plasma ─────────────────────────────────────────────────
       Four Bhaskara-approximated sine waves at different spatial
       frequencies interfere to produce smooth, organic swirling color. */
    for (int y = 21; y < 310; y++) {
        int ry = y - 21;
        for (int x = 0; x < 800; x++) {
            int32_t v = gfx_sin( x       * 256 / 310)
                      + gfx_sin( ry      * 256 / 200 + 64)
                      + gfx_sin((x + ry) * 256 / 360)
                      + gfx_sin((x - ry + 512) * 256 / 280 + 128);
            /* v ∈ [-508,508]; double-cycle through spectrum for richness */
            uint32_t hue = (uint32_t)((v + 508) * 3071 / 1016) % 1536u;
            vga_pixel(x, y, hsv_to_rgb(hue, 240, 255));
        }
    }

    /* ── Divider ──────────────────────────────────────────────────── */
    vga_hline(0, 310, 800, 0x080E18);
    vga_hline(0, 311, 800, 0x152030);

    /* ── Mandelbrot (left 400 columns, y 312..599) ────────────────── */
    for (int py = 0; py < 288; py++)
        for (int px = 0; px < 400; px++)
            vga_pixel(px, 312 + py, gfx_mandel(px, py, 400, 288));

    /* ── Burning Ship (right 400 columns, y 312..599) ────────────── */
    for (int py = 0; py < 288; py++)
        for (int px = 0; px < 400; px++)
            vga_pixel(400 + px, 312 + py, gfx_burning_ship(px, py, 400, 288));

    /* ── Vertical divider ─────────────────────────────────────────── */
    vga_vline(400, 310, 290, 0x080E18);

    /* ── Panel labels ─────────────────────────────────────────────── */
    vga_rect(4,   21,  56, 13, 0x08101C);
    vga_print_gfx(6,  23, "PLASMA", 0x88CCFF);

    vga_rect(4,   312, 118, 13, 0x04080F);
    vga_print_gfx(6,  313, "MANDELBROT SET", 0x88CCFF);

    vga_rect(402, 312, 128, 13, 0x0F0800);
    vga_print_gfx(404, 313, "BURNING SHIP", 0xFFAA44);

    /* ── Footer ───────────────────────────────────────────────────── */
    vga_rect(216, 586, 368, 12, 0x020406);
    vga_print_gfx(220, 587, "Press any key to return to shell", 0x334455);

    while (keyboard_haschar()) keyboard_getchar();
    while (!keyboard_haschar()) __asm__ volatile("hlt");
    keyboard_getchar();

    vga_redraw();
}

void shell_dispatch(char* line)
{
    while (*line == ' ') line++;
    if (!*line) return;
    char* cmd = line;
    char* args = line;
    while (*args && *args != ' ') args++;
    if (*args == ' ') { *args = '\0'; args++; }
    while (*args == ' ') args++;   /* skip multiple spaces between cmd and args */

    if      (str_eq(cmd, "fetch"))    cmd_fetch();
    else if (str_eq(cmd, "mem"))      cmd_mem();
    else if (str_eq(cmd, "memstat"))  cmd_memstat();
    else if (str_eq(cmd, "memdump"))  cmd_memdump(args);
    else if (str_eq(cmd, "leaks"))    cmd_leaks();
    else if (str_eq(cmd, "uptime"))   cmd_uptime();
    else if (str_eq(cmd, "dmesg"))    cmd_dmesg();
    else if (str_eq(cmd, "beep"))     cmd_beep(args);
    else if (str_eq(cmd, "play"))     cmd_play(args);
    else if (str_eq(cmd, "version"))  cmd_version();
    else if (str_eq(cmd, "ps"))       cmd_ps();
    else if (str_eq(cmd, "clear"))    cmd_clear();
    else if (str_eq(cmd, "echo"))     cmd_echo(args);
    else if (str_eq(cmd, "disk"))     cmd_disk();
    else if (str_eq(cmd, "lspci"))    cmd_lspci();
    else if (str_eq(cmd, "netinfo"))  cmd_netinfo();
    else if (str_eq(cmd, "netsend"))  cmd_netsend();
    else if (str_eq(cmd, "dhcp"))     cmd_dhcp();
    else if (str_eq(cmd, "nslookup")) cmd_nslookup(args);
    else if (str_eq(cmd, "ping"))     cmd_ping(args);
    else if (str_eq(cmd, "udpsend"))  cmd_udpsend(args);
    else if (str_eq(cmd, "wget"))     cmd_wget(args);
    else if (str_eq(cmd, "browser"))  cmd_browser(args);
    else if (str_eq(cmd, "fsinfo"))   cmd_fsinfo();
    else if (str_eq(cmd, "diskread")) cmd_diskread(args);
    else if (str_eq(cmd, "gfx"))      cmd_gfx();
    else if (str_eq(cmd, "gui"))      wm_run();
    else if (str_eq(cmd, "shutdown")) cmd_shutdown();
    else if (str_eq(cmd, "poweroff")) cmd_shutdown();
    else if (str_eq(cmd, "reboot"))   cmd_reboot();
    else if (str_eq(cmd, "halt"))     cmd_halt();
    else if (str_eq(cmd, "ls"))       cmd_ls(args);
    else if (str_eq(cmd, "pwd"))      cmd_pwd();
    else if (str_eq(cmd, "cd"))       cmd_cd(args);
    else if (str_eq(cmd, "mkdir"))    cmd_mkdir(args);
    else if (str_eq(cmd, "cat"))      cmd_cat(args);
    else if (str_eq(cmd, "write"))    cmd_write(args);
    else if (str_eq(cmd, "touch"))    cmd_touch(args);
    else if (str_eq(cmd, "cp"))       cmd_cp(args);
    else if (str_eq(cmd, "mv"))       cmd_mv(args);
    else if (str_eq(cmd, "rm"))       cmd_rm(args);
    else if (str_eq(cmd, "edit"))     cmd_edit(args);
    else if (str_eq(cmd, "exec"))     cmd_exec(args);
    else if (str_eq(cmd, "info"))     cmd_info();
    else if (str_eq(cmd, "help"))     cmd_help();
    else {
        vga_print("  Unknown command.\n");
        /* Log unknown commands so they show up in dmesg */
        static char uk_log[48];
        static const char uk_px[] = "[SHELL] unknown: ";
        int ui = 0;
        for (; uk_px[ui]; ui++) uk_log[ui] = uk_px[ui];
        for (int j = 0; cmd[j] && ui < 46; j++) uk_log[ui++] = cmd[j];
        uk_log[ui] = '\0';
        klog(LOG_WARN, uk_log);
    }
}

/* ── SHELL v2 LINE EDITOR HELPERS ───────────────────────────────── */

/* Move the VGA cursor to input-buffer position pos. */
static void sh_goto_pos(uint32_t pos)
{
    int abs_pos = sh_pr_col + (int)pos;
    int row = sh_pr_row + abs_pos / PT_COLS;
    int col = abs_pos % PT_COLS;
    if (row >= PT_ROWS) row = PT_ROWS - 1;
    vga_goto((uint8_t)row, (uint8_t)col);
}

/* Redraw the input line on screen.
   Goes to the prompt's input-start position, reprints the whole buffer,
   clears any surplus characters from the old (longer) line, then
   repositions the cursor at cur_pos. */
static void sh_redraw(const char *buf, uint32_t new_len,
                      uint32_t old_len, uint32_t cur_pos)
{
    /* Write max(new_len, old_len) + 1 characters total: the new content
       followed by enough spaces to fully erase any previous content.
       The +1 guards against any off-by-one in the erase boundary. */
    uint32_t total = (new_len > old_len ? new_len : old_len) + 1u;
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_goto((uint8_t)sh_pr_row, (uint8_t)sh_pr_col);
    for (uint32_t i = 0; i < total; i++) {
        if (i < new_len) vga_putchar(buf[i]);
        else             vga_putchar(' ');
    }
    sh_goto_pos(cur_pos);
}

/* ── TAB COMPLETION ─────────────────────────────────────────────── */
static const char * const sh_cmds[] = {
    "beep",     "cat",      "cd",       "clear",    "cp",
    "dhcp",     "disk",     "diskread", "dmesg",    "echo",
    "edit",     "exec",     "fetch",    "fsinfo",   "gfx",
    "gui",      "halt",     "help",     "info",     "ls",       "lspci",
    "leaks",    "mem",      "memdump",  "memstat",  "mkdir",
    "mv",       "netinfo",  "netsend",
    "nslookup", "ping",     "poweroff", "ps",       "pwd",
    "reboot",   "rm",       "shutdown", "touch",    "udpsend",
    "browser",  "uptime",   "version",  "wget",     "write",    (void*)0
};

static void sh_tab_complete(char *buf, uint32_t *plen, uint32_t *ppos)
{
    uint32_t pos = *ppos;
    uint32_t len = *plen;
    if (pos != len) return;     /* only complete at end of line */

    const char *match = (void*)0;
    int count = 0;
    for (int i = 0; sh_cmds[i]; i++) {
        int ok = 1;
        for (uint32_t j = 0; j < pos; j++) {
            if (!sh_cmds[i][j] || sh_cmds[i][j] != buf[j]) { ok = 0; break; }
        }
        if (ok) { match = sh_cmds[i]; count++; }
    }

    if (count == 1) {
        /* Complete + append space */
        uint32_t old_len = len;
        len = 0;
        while (match[len] && len < 253) { buf[len] = match[len]; len++; }
        buf[len++] = ' ';
        buf[len]   = '\0';
        pos = len;
        sh_redraw(buf, len, old_len, pos);
        *plen = len; *ppos = pos;
    } else if (count > 1) {
        /* Print all matches, then reprint prompt + current input */
        vga_print("\n");
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_print("  ");
        for (int i = 0; sh_cmds[i]; i++) {
            int ok = 1;
            for (uint32_t j = 0; j < pos; j++) {
                if (!sh_cmds[i][j] || sh_cmds[i][j] != buf[j]) { ok = 0; break; }
            }
            if (ok) { vga_print(sh_cmds[i]); vga_print("  "); }
        }
        vga_print("\n");
        shell_prompt();
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        for (uint32_t i = 0; i < len; i++) vga_putchar(buf[i]);
    }
}

/* ── SHELL v2 MAIN LOOP ─────────────────────────────────────────── */
void shell_run(void)
{
    static char input[256];
    static char saved_line[256];
    uint32_t len      = 0;
    int      hist_idx = 0;

    shell_prompt();
    for (;;) {
        __asm__ volatile ("hlt");
        while (keyboard_haschar()) {
            uint8_t c = (uint8_t)keyboard_getchar();

            /* UP / DOWN: scroll the terminal — do NOT snap to live first */
            if (c == KEY_UP)   { vga_view_up();   continue; }
            if (c == KEY_DOWN) { vga_view_down();  continue; }

            /* All other keys: snap to live screen before editing */
            vga_view_reset();

            if (c == '\n') {
                input[len] = '\0';
                vga_print("\n");
                if (len > 0) {
                    hist_push(input);
                    shell_dispatch(input);
                }
                len = 0; hist_idx = 0;
                shell_prompt();

            } else if (c == '\b') {
                if (len > 0) {
                    len--;
                    /* Redraw: overwrite the now-shorter line + clear the tail char */
                    sh_redraw(input, len, len + 1, len);
                }

            } else if (c == KEY_LEFT) {
                /* History: go to older (previous) entry */
                if (hist_idx == 0) {
                    for (uint32_t i = 0; i <= len; i++) saved_line[i] = input[i];
                }
                if (hist_idx < hist_count) {
                    uint32_t old_len = len;
                    hist_idx++;
                    const char *h = hist_get(hist_idx);
                    if (h) {
                        for (len = 0; h[len] && len < 255; len++) input[len] = h[len];
                        input[len] = '\0';
                        sh_redraw(input, len, old_len, len);
                    }
                }

            } else if (c == KEY_RIGHT) {
                /* History: go to newer (next) entry / restore saved line */
                if (hist_idx > 0) {
                    uint32_t old_len = len;
                    hist_idx--;
                    if (hist_idx == 0) {
                        for (len = 0; saved_line[len] && len < 255; len++)
                            input[len] = saved_line[len];
                        input[len] = '\0';
                    } else {
                        const char *h = hist_get(hist_idx);
                        if (h) {
                            for (len = 0; h[len] && len < 255; len++) input[len] = h[len];
                            input[len] = '\0';
                        }
                    }
                    sh_redraw(input, len, old_len, len);
                }

            } else if (c == 0x03) {
                /* Ctrl+C: cancel current line */
                vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
                vga_print("^C\n");
                len = 0; hist_idx = 0;
                shell_prompt();

            } else if (c == '\t') {
                sh_tab_complete(input, &len, &len);

            } else if (c >= 0x20u && c < 0x80u && len < 255) {
                /* Printable: append at end */
                input[len++] = (char)c;
                input[len]   = '\0';
                vga_putchar((char)c);
            }
        }
    }
}
