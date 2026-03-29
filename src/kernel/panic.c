/* IronKernel — panic.c
   Full-screen panic renderer.  Draws directly to the framebuffer
   (bypassing the WM double-buffer) then halts the CPU. */

#include "panic.h"
#include "../drivers/vga.h"
#include "types.h"
#include "klog.h"
#include "../drivers/serial.h"
#include "../drivers/speaker.h"

/* ── helpers ─────────────────────────────────────────────────────── */

static void u64_hex(char *buf, uint64_t v)
{
    /* Write exactly 16 hex digits into buf[0..15]; buf[16] = '\0'. */
    const char *h = "0123456789ABCDEF";
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
}

static void u32_hex(char *buf, uint32_t v)
{
    const char *h = "0123456789ABCDEF";
    buf[8] = '\0';
    for (int i = 7; i >= 0; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
}

static const char *exc_short(uint32_t n)
{
    static const char *tbl[] = {
        "#DE",  "#DB",  "NMI",  "#BP",  "#OF",  "#BR",  "#UD",  "#NM",
        "#DF",  "cso",  "#TS",  "#NP",  "#SS",  "#GP",  "#PF",  "res",
        "#MF",  "#AC",  "#MC",  "#XF",  "#VE",  "#CP",
    };
    if (n < 22) return tbl[n];
    return "#??";
}

static const char *exc_long(uint32_t n)
{
    static const char *tbl[] = {
        "Divide-by-Zero",       "Debug",                "NMI",
        "Breakpoint",           "Overflow",             "Bound Range",
        "Invalid Opcode",       "Device Not Available", "Double Fault",
        "Coprocessor Overrun",  "Invalid TSS",          "Segment Not Present",
        "Stack-Segment Fault",  "General Protection",   "Page Fault",
        "Reserved",             "x87 Floating-Point",   "Alignment Check",
        "Machine Check",        "SIMD Floating-Point",  "Virtualization",
        "Control Protection",
    };
    if (n < 22) return tbl[n];
    return "Unknown";
}

/* Print a string with vga_print_gfx. */
static void pp(int x, int y, const char *s, uint32_t col)
{
    vga_print_gfx(x, y, s, col);
}

/* Print one line: label + 0x + 16 hex digits. */
static void pp_reg(int x, int y, const char *label, uint64_t val, uint32_t col)
{
    char buf[20];
    pp(x, y, label, 0xCCCCCC);
    buf[0] = '0'; buf[1] = 'x';
    u64_hex(buf + 2, val);
    buf[18] = '\0';
    pp(x + (int)(8 * 6), y, buf, col);
}

/* ── stack-trace walker ──────────────────────────────────────────── */

static void draw_stacktrace(int x, int *py, uint64_t rbp)
{
    pp(x, *py, "STACK TRACE:", 0xFFCC44);
    *py += 12;

    char buf[24];
    int frames = 0;
    uint64_t *bp = (uint64_t *)(uintptr_t)rbp;

    /* Walk RBP chain.  Each frame: [0] = saved RBP, [1] = return address. */
    while (bp && frames < 8) {
        /* Guard: only walk kernel-space addresses (bit 63 set or > 1MB) */
        if ((uintptr_t)bp < 0x100000) break;

        uint64_t ret = bp[1];
        if (!ret) break;

        buf[0] = ' '; buf[1] = '#';
        buf[2] = '0' + (char)frames;
        buf[3] = ' '; buf[4] = ' ';
        buf[5] = '0'; buf[6] = 'x';
        u64_hex(buf + 7, ret);
        /* u64_hex writes 16 chars + null at buf[23]; total 24 bytes needed */
        pp(x, *py, buf, 0xAABBCC);
        *py += 11;
        frames++;

        bp = (uint64_t *)(uintptr_t)bp[0];
    }
    if (frames == 0)
        pp(x, *py, "  (trace unavailable)", 0x778899);
    *py += 14;
}

/* ── main panic entry ────────────────────────────────────────────── */

void panic_ex(const char *msg,
              uint64_t rip, uint64_t rsp, uint64_t rbp,
              uint64_t err, uint64_t cr2,
              uint32_t intnum, int is_user)
{
    __asm__ volatile("cli");

    /* Record the panic in the log before we overwrite the screen. */
    klog(LOG_PANIC, msg);

    /* Mirror panic message to COM1 serial for post-mortem log analysis. */
    serial_puts("\r\n=== KERNEL PANIC ===\r\n");
    serial_puts(msg);
    serial_puts("\r\n");
    {
        /* Write RIP as 16 hex digits — use a local copy to avoid clobbering rip */
        const char *hx = "0123456789ABCDEF";
        char rbuf[23]; rbuf[0]='R'; rbuf[1]='I'; rbuf[2]='P'; rbuf[3]='=';
        rbuf[4]='0'; rbuf[5]='x';
        uint64_t tmp = rip;
        for (int _i = 15; _i >= 0; _i--) { rbuf[6+_i] = hx[tmp & 0xF]; tmp >>= 4; }
        rbuf[22] = '\0';
        serial_puts(rbuf);
        serial_puts("\r\n===================\r\n");
    }

    /* Force all pixel writes to the real framebuffer (not back buffer). */
    vga_panic_setup();

    /* ── Background ── */
    vga_gfx_clear(0x1A0000);           /* deep red */
    vga_rect(0, 0, 800, 38, 0xCC0000); /* header bar */
    vga_rect(0, 38, 800, 2, 0xFF2222); /* bright separator */

    /* ── Header ── */
    pp(10, 10, "!!  KERNEL PANIC  !!", 0xFFFFFF);
    pp(300, 10, "IronKernel", 0xFF8888);

    /* ── Exception or manual panic? ── */
    int y = 50;
    char nbuf[12];

    if (intnum != 0xFFFFFFFFu) {
        /* From ISR — show exception type */
        u32_hex(nbuf, intnum);
        pp(10, y, "EXCEPTION 0x", 0xFF6644);
        pp(10 + 12 * 8, y, nbuf, 0xFF9966);
        pp(10 + (12 + 8 + 2) * 8, y,
           exc_short(intnum), 0xFFBB88);
        pp(10 + (12 + 8 + 2 + 5) * 8, y,
           exc_long(intnum), 0xFFCC99);
        y += 14;
        if (is_user)
            pp(10, y, "(fault from user-mode code)", 0xFF9966);
        y += 16;
    } else {
        pp(10, y, "EXPLICIT PANIC", 0xFF6644);
        y += 20;
    }

    /* ── Panic message ── */
    vga_rect(8, y - 2, 784, 14, 0x3A0000);
    pp(10, y, msg, 0xFFFF88);
    y += 22;

    /* ── Register block ── */
    vga_rect(8, y - 2, 784, 2, 0x660000);
    y += 4;

    /* Two columns: left = RIP/RSP/RBP, right = CR2/ERR/INT */
    pp_reg( 10, y,      "RIP  ", rip, 0xFFEE88);
    pp_reg(410, y,      "CR2  ", cr2, (intnum == 14) ? 0xFF8888 : 0xAABBCC);
    y += 13;
    pp_reg( 10, y,      "RSP  ", rsp, 0xCCDDEE);
    pp_reg(410, y,      "ERR  ", err, 0xCCDDEE);
    y += 13;
    pp_reg( 10, y,      "RBP  ", rbp, 0xAABBCC);
    {
        char ibuf[12];
        u32_hex(ibuf, intnum);
        pp(410, y, "INT  0x", 0xCCCCCC);
        pp(410 + 7 * 8, y,
           (intnum == 0xFFFFFFFFu) ? "--------" : ibuf, 0xAABBCC);
    }
    y += 20;

    /* ── Stack trace (kernel mode only) ── */
    if (!is_user) {
        draw_stacktrace(10, &y, rbp);
    } else {
        pp(10, y, "Stack trace: n/a (user-mode fault)", 0x778899);
        y += 14;
    }

    /* ── Footer ── */
    vga_rect(0, 590, 800, 10, 0xCC0000);
    pp(10, 591, "System halted.  No further execution possible.", 0xFFFFFF);

    /* ── Panic sound ── */
    /* Re-enable interrupts briefly so PIT ticks advance for beep timing,
       then disable permanently and halt. */
    __asm__ volatile("sti");
    speaker_panic_sound();
    __asm__ volatile("cli");

    /* ── Halt ── */
    for (;;) __asm__ volatile("hlt");
}
