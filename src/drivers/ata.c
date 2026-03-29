#include "ata.h"
#include "vga.h"
#include "../kernel/types.h"
#include "../kernel/klog.h"

/* ── PORT I/O ───────────────────────────────────────────────────── */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %1, %0" : : "dN"(port), "a"(val));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "dN"(port));
    return ret;
    /* Read a 16-bit word from a port.
       ATA data transfers use 16-bit reads/writes — the data port
       is 16 bits wide. All other ATA ports are 8-bit. */
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %1, %0" : : "dN"(port), "a"(val));
}

/* ── GLOBAL DRIVE DESCRIPTOR ────────────────────────────────────── */

ata_drive_t ata_master;

/* ── ATA DELAYS ─────────────────────────────────────────────────── */

/* 400 ns delay: read the alternate status register 4 times.
   ATA spec requires at least 400 ns after drive select or SRST
   before reading the status register. Each port read ≈ 100 ns.
   Use ATA_PRIMARY_CTRL (0x3F6) — reading it does NOT clear pending IRQs. */
static void ata_io_delay(void)
{
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
}

/* ── ATA WAIT ───────────────────────────────────────────────────── */

static int ata_wait_ready(void)
{
    uint8_t status;
    uint32_t timeout = 0x100000; /* ~1M polls — handles slow QEMU hosts */
    while (timeout--) {
        status = inb(ATA_PRIMARY_STATUS);
        if (status == 0xFF) return -1;   /* floating bus — no drive */
        if (status & ATA_SR_ERR)  return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) return 0;
    }
    return -1;
}

static int ata_wait_drq(void)
{
    uint8_t status;
    uint32_t timeout = 0x100000;
    while (timeout--) {
        status = inb(ATA_PRIMARY_STATUS);
        if (status == 0xFF) return -1;
        if (status & ATA_SR_ERR)          return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

/* ── IDENTIFY ───────────────────────────────────────────────────── */

static int ata_identify(void)
{
    /* Select master drive, then wait 400 ns for the selection to settle
       before accessing any other registers (ATA spec requirement). */
    outb(ATA_PRIMARY_DRIVE_HEAD, 0xA0);
    ata_io_delay();

    /* Wait for the drive to be ready (BSY=0, DRDY=1) before issuing
       any command — required by ATA spec. */
    if (ata_wait_ready() != 0) return -1;

    /* Zero sector count and LBA registers before IDENTIFY. */
    outb(ATA_PRIMARY_SECCOUNT, 0);
    outb(ATA_PRIMARY_LBA_LO,   0);
    outb(ATA_PRIMARY_LBA_MID,  0);
    outb(ATA_PRIMARY_LBA_HI,   0);

    /* Send IDENTIFY command. */
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_IDENTIFY);
    ata_io_delay();   /* 400 ns before first status read after command */

    /* Read status — 0x00 or 0xFF means no drive. */
    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status == 0x00 || status == 0xFF) return -1;

    /* Wait for BSY to clear after IDENTIFY. */
    uint32_t timeout = 0x100000;
    while (timeout--) {
        status = inb(ATA_PRIMARY_STATUS);
        if (status == 0xFF) return -1;
        if (!(status & ATA_SR_BSY)) break;
    }
    if (status & ATA_SR_BSY) return -1;
    /* BSY never cleared — drive did not respond. Check actual status
       rather than timeout counter because the counter wraps to 0xFFFFFFFF
       on exhaustion, making a == 0 check unreliable. */

    /* Check LBA_MID and LBA_HI — if non-zero, not an ATA drive. */
    if (inb(ATA_PRIMARY_LBA_MID) || inb(ATA_PRIMARY_LBA_HI)) return -1;

    /* Wait for DRQ or ERR. */
    if (ata_wait_drq() != 0) return -1;

    /* Read 256 words of IDENTIFY data. */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++) {
        identify[i] = inw(ATA_PRIMARY_DATA);
    }

    /* Extract 28-bit LBA sector count from words 60–61.
       Word 60 = low 16 bits, word 61 = high 16 bits. */
    ata_master.sectors = ((uint32_t)identify[61] << 16) | identify[60];

    /* Extract model string from words 27–46.
       ATA model strings are 40 bytes stored as 20 words.
       Each word has its bytes swapped — we un-swap them. */
    for (int i = 0; i < 20; i++) {
        ata_master.model[i * 2]     = (char)(identify[27 + i] >> 8);
        ata_master.model[i * 2 + 1] = (char)(identify[27 + i] & 0xFF);
    }
    ata_master.model[40] = '\0';

    /* Trim trailing spaces from model string. */
    for (int i = 39; i >= 0 && ata_master.model[i] == ' '; i--) {
        ata_master.model[i] = '\0';
    }

    ata_master.present = 1;
    return 0;
}

/* ── SETUP LBA28 REGISTERS ──────────────────────────────────────── */

static void ata_setup_lba(uint32_t lba, uint8_t count)
{
    outb(ATA_PRIMARY_DRIVE_HEAD,
         0xE0 | ((lba >> 24) & 0x0F));
    ata_io_delay();   /* 400 ns after drive select */

    outb(ATA_PRIMARY_ERROR,   0x00);
    /* Clear features register before command. */

    outb(ATA_PRIMARY_SECCOUNT, count);
    /* Number of sectors to transfer. */

    outb(ATA_PRIMARY_LBA_LO,  (uint8_t)(lba & 0xFF));
    outb(ATA_PRIMARY_LBA_MID, (uint8_t)((lba >> 8)  & 0xFF));
    outb(ATA_PRIMARY_LBA_HI,  (uint8_t)((lba >> 16) & 0xFF));
    /* Write the three LBA bytes to their respective registers.
       Together with bits 24-27 in DRIVE_HEAD, this forms the
       complete 28-bit LBA sector address. */
}

/* ── PUBLIC FUNCTIONS ───────────────────────────────────────────── */

void ata_init(void)
{
    ata_master.present = 0;
    ata_master.sectors = 0;
    ata_master.model[0] = '\0';

    /* Software reset + IDENTIFY with up to 3 attempts.
       QEMU's emulated IDE drive sometimes takes an extra moment after
       a fast guest boot before it becomes responsive — a single attempt
       can fail even though the drive is physically present.
       Each attempt: assert SRST, deassert, wait for BSY to clear,
       then IDENTIFY.  Re-resetting between retries is harmless. */
    for (int _try = 0; _try < 3; _try++) {
        outb(ATA_PRIMARY_CTRL, 0x04);   /* assert SRST (bit 2) */
        ata_io_delay();                 /* ≥400 ns asserted     */
        outb(ATA_PRIMARY_CTRL, 0x00);   /* deassert SRST        */

        /* Wait for BSY to clear after reset.  ~100 ns per poll × 1M = 100 ms max. */
        uint32_t _t = 0x100000;
        while (_t--) {
            if (!(inb(ATA_PRIMARY_CTRL) & ATA_SR_BSY)) break;
        }

        if (ata_identify() == 0) break; /* success — stop retrying */
    }

    if (!ata_master.present) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_print("[ATA] No drive detected on primary bus.\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        klog(LOG_WARN, "[ATA] no drive on primary bus");
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("[ATA] DRIVE DETECTED\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("      Model   : ");
    vga_print(ata_master.model[0] ? ata_master.model : "(unknown)");
    vga_print("\n");
    /* Log drive info: "[ATA] <model>" */
    {
        char kbuf[80];
        int ki = 0;
        const char *pfx = "[ATA] ";
        while (pfx[ki] && ki < 6) { kbuf[ki] = pfx[ki]; ki++; }
        const char *mdl = ata_master.model[0] ? ata_master.model : "(unknown)";
        for (int j = 0; mdl[j] && ki < 78; j++) kbuf[ki++] = mdl[j];
        kbuf[ki] = '\0';
        klog(LOG_INFO, kbuf);
    }
    vga_print("      Sectors : ");

    /* Print sector count. */
    char buf[12]; buf[11] = '\0'; int i = 10;
    uint32_t s = ata_master.sectors;
    if (s == 0) { vga_print("0"); }
    else {
        while (s > 0 && i >= 0) { buf[i--] = '0' + (s % 10); s /= 10; }
        vga_print(&buf[i+1]);
    }
    vga_print("  (");

    /* Print size in MB. */
    buf[11] = '\0'; i = 10;
    uint32_t mb = ata_master.sectors / 2048;
    if (mb == 0) { vga_print("0"); }
    else {
        while (mb > 0 && i >= 0) { buf[i--] = '0' + (mb % 10); mb /= 10; }
        vga_print(&buf[i+1]);
    }
    vga_print(" MB)\n");
}

int ata_read_sector(uint32_t lba, uint8_t* buf)
{
    if (!ata_master.present) return -1;

    if (ata_wait_ready() != 0) return -1;

    ata_setup_lba(lba, 1);
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_PIO);
    /* Send READ SECTORS command — drive begins seeking. */

    if (ata_wait_drq() != 0) return -1;
    /* Wait until drive has the sector data ready for us to read. */

    uint16_t* ptr = (uint16_t*)buf;
    for (int i = 0; i < 256; i++) {
        ptr[i] = inw(ATA_PRIMARY_DATA);
        /* Read 256 words = 512 bytes into the caller's buffer.
           Must read all 256 words — drive resets DRQ after the last. */
    }

    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t* buf)
{
    if (!ata_master.present) return -1;

    if (ata_wait_ready() != 0) return -1;

    ata_setup_lba(lba, 1);
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_PIO);

    if (ata_wait_drq() != 0) return -1;
    /* Wait until drive is ready to accept our data. */

    const uint16_t* ptr = (const uint16_t*)buf;
    for (int i = 0; i < 256; i++) {
        outw(ATA_PRIMARY_DATA, ptr[i]);
        /* Write 256 words = 512 bytes to the drive.
           Drive buffers the data internally and commits to disk. */
    }

    /* Flush drive write cache — ensures data is committed. */
    outb(ATA_PRIMARY_COMMAND, 0xE7);
    /* Command 0xE7 = FLUSH CACHE.
       Without this, data may sit in the drive's write buffer
       and be lost if power is cut before it is written. */
    ata_wait_ready();

    return 0;
}
