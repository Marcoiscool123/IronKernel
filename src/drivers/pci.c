/* IronKernel — pci.c
   PCI bus driver: enumerates devices via config space I/O ports 0xCF8/0xCFC. */

#include "pci.h"

/* ── PORT I/O ────────────────────────────────────────────────────── */

static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %1,%0" :: "Nd"(port), "a"(val));
}
static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ── CONFIG SPACE ACCESS ─────────────────────────────────────────── */

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (reg & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

/* ── DEVICE TABLE ────────────────────────────────────────────────── */

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static int          pci_dev_count = 0;

/* ── SCAN ────────────────────────────────────────────────────────── */

void pci_init(void)
{
    pci_dev_count = 0;

    for (uint16_t bus = 0; bus < 256 && pci_dev_count < PCI_MAX_DEVICES; bus++) {
        for (uint8_t dev = 0; dev < 32 && pci_dev_count < PCI_MAX_DEVICES; dev++) {

            /* Check function 0 first — if no device, skip all functions */
            uint32_t id0 = pci_read32((uint8_t)bus, dev, 0, 0x00);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;

            /* Check multifunction bit in header type */
            uint32_t hdr0   = pci_read32((uint8_t)bus, dev, 0, 0x0C);
            uint8_t  htype0 = (hdr0 >> 16) & 0xFF;
            int      max_fn = (htype0 & 0x80) ? 8 : 1;

            for (int fn = 0; fn < max_fn && pci_dev_count < PCI_MAX_DEVICES; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, (uint8_t)fn, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue; /* empty function slot */

                uint32_t cc  = pci_read32((uint8_t)bus, dev, (uint8_t)fn, 0x08);
                uint32_t hdr = pci_read32((uint8_t)bus, dev, (uint8_t)fn, 0x0C);

                pci_devices[pci_dev_count++] = (pci_device_t){
                    .bus         = (uint8_t)bus,
                    .dev         = dev,
                    .fn          = (uint8_t)fn,
                    .vendor_id   = (uint16_t)(id & 0xFFFF),
                    .device_id   = (uint16_t)(id >> 16),
                    .class_code  = (uint8_t)(cc >> 24),
                    .subclass    = (uint8_t)(cc >> 16),
                    .prog_if     = (uint8_t)(cc >> 8),
                    .header_type = (uint8_t)(hdr >> 16),
                };
            }
        }
    }
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val)
{
    uint32_t addr = 0x80000000u
                  | ((uint32_t)bus << 16)
                  | ((uint32_t)dev << 11)
                  | ((uint32_t)fn  <<  8)
                  | (reg & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

/* ── PUBLIC ACCESSORS ────────────────────────────────────────────── */

int pci_count(void) { return pci_dev_count; }

const pci_device_t *pci_get(int idx)
{
    if (idx < 0 || idx >= pci_dev_count) return (void*)0;
    return &pci_devices[idx];
}

const pci_device_t *pci_find_device(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < pci_dev_count; i++) {
        if (pci_devices[i].vendor_id == vendor &&
            pci_devices[i].device_id == device)
            return &pci_devices[i];
    }
    return (void*)0;
}
