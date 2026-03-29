#ifndef ATA_H
#define ATA_H

#include "../kernel/types.h"

/* ── ATA PRIMARY BUS I/O PORTS ──────────────────────────────────────
   The ATA primary bus has been at these port addresses on every
   IBM PC-compatible since the PC/AT in 1984. Secondary bus is
   at 0x170. We implement primary only for now.
   ─────────────────────────────────────────────────────────────── */

#define ATA_PRIMARY_DATA        0x1F0
/* 16-bit data port. Read/write 256 words (512 bytes) per sector.
   Every sector transfer goes through this port word by word. */

#define ATA_PRIMARY_ERROR       0x1F1
/* Read:  error register — bits describe what went wrong.
   Write: features register — select optional drive features. */

#define ATA_PRIMARY_SECCOUNT    0x1F2
/* Number of sectors to transfer (1–255). We always use 1. */

#define ATA_PRIMARY_LBA_LO      0x1F3
/* LBA bits 0–7 — least significant byte of sector address. */

#define ATA_PRIMARY_LBA_MID     0x1F4
/* LBA bits 8–15. */

#define ATA_PRIMARY_LBA_HI      0x1F5
/* LBA bits 16–23. */

#define ATA_PRIMARY_DRIVE_HEAD  0x1F6
/* Drive and head select register.
   Bits 7,5 : always 1 (legacy requirement)
   Bit  6   : 1 = LBA mode, 0 = CHS mode (we always use LBA)
   Bit  4   : 0 = master drive, 1 = slave drive
   Bits 3-0 : LBA bits 24–27 (top 4 bits of 28-bit LBA) */

#define ATA_PRIMARY_STATUS      0x1F7
/* Read:  status register
   Bit 7 = BSY  (busy — controller is working, do not touch)
   Bit 6 = DRDY (drive ready)
   Bit 4 = DSC  (drive seek complete)
   Bit 3 = DRQ  (data request — ready to transfer data)
   Bit 0 = ERR  (error occurred — check error register) */

#define ATA_PRIMARY_COMMAND     0x1F7
/* Write: command register — same port as status.
   Writing here sends a command to the drive. */

#define ATA_PRIMARY_CTRL        0x3F6
/* Alternate status / device control register.
   Writing bit 2 = 1 resets the drive.
   Reading gives status without clearing pending interrupts. */

/* ── ATA STATUS BITS ────────────────────────────────────────────── */

#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DRQ   0x08
#define ATA_SR_ERR   0x01

/* ── ATA COMMANDS ───────────────────────────────────────────────── */

#define ATA_CMD_READ_PIO   0x20
/* Read sectors using PIO — CPU reads data word by word from port. */

#define ATA_CMD_WRITE_PIO  0x30
/* Write sectors using PIO — CPU writes data word by word to port. */

#define ATA_CMD_IDENTIFY   0xEC
/* IDENTIFY DEVICE — drive returns 512 bytes of information:
   model string, serial number, sector count, capabilities.
   We use this to detect drive presence and size. */

/* ── SECTOR SIZE ────────────────────────────────────────────────── */

#define ATA_SECTOR_SIZE  512
/* Every ATA sector is exactly 512 bytes — 256 16-bit words.
   This has been true since the first ATA drives in 1986.
   4KB native sectors exist on modern drives but are abstracted
   behind a 512-byte emulation layer for compatibility. */

/* ── DRIVE DESCRIPTOR ───────────────────────────────────────────── */

typedef struct {
    uint8_t  present;
    /* 1 = drive detected and responding, 0 = not present. */

    uint32_t sectors;
    /* Total number of 512-byte sectors on the drive.
       Derived from IDENTIFY data words 60–61 (28-bit LBA count). */

    char     model[41];
    /* Model string from IDENTIFY data — null terminated.
       Bytes in IDENTIFY are byte-swapped per ATA spec — we fix this. */

} ata_drive_t;

/* ── PUBLIC INTERFACE ───────────────────────────────────────────── */

void ata_init(void);
/* Probe the primary ATA bus. Identify master drive if present.
   Populate the global ata_master descriptor.
   Must be called before ata_read_sector or ata_write_sector. */

int ata_read_sector(uint32_t lba, uint8_t* buf);
/* Read one 512-byte sector at LBA address into buf.
   Returns 0 on success, -1 on error or drive not present.
   buf must point to at least 512 bytes of writable memory. */

int ata_write_sector(uint32_t lba, const uint8_t* buf);
/* Write one 512-byte sector from buf to LBA address on disk.
   Returns 0 on success, -1 on error or drive not present.
   Data is committed to the drive before the function returns. */

extern ata_drive_t ata_master;
/* Global descriptor for the primary master drive.
   Populated by ata_init. Read by shell disk commands. */

#endif
