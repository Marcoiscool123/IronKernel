#ifndef PCI_H
#define PCI_H

#include "../kernel/types.h"

/* ── PCI DEVICE RECORD ───────────────────────────────────────────── */

typedef struct {
    uint8_t  bus, dev, fn;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if;
    uint8_t  header_type;
} pci_device_t;

#define PCI_MAX_DEVICES 64

/* ── PUBLIC API ──────────────────────────────────────────────────── */

void               pci_init(void);
/* Scan all PCI buses/devices/functions and populate the device table. */

uint32_t           pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
/* Read a 32-bit word from PCI config space at the given address. */

void               pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val);
/* Write a 32-bit word to PCI config space at the given address. */

int                pci_count(void);
/* Return number of devices found by pci_init(). */

const pci_device_t *pci_get(int idx);
/* Return pointer to device record at index idx (0-based). */

const pci_device_t *pci_find_device(uint16_t vendor, uint16_t device);
/* Find first device matching vendor+device ID; NULL if not found. */

#endif
