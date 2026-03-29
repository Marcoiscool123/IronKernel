/* IronKernel — e1000.c
   Intel 82540EM NIC driver.  Uses legacy PIC IRQs and PIO-style MMIO
   (BAR0 identity-mapped into the kernel's high page table region).
   TX: 8-slot ring, busy-wait DD poll.
   RX: 8-slot ring, polled from shell commands or IRQ wakeup. */

#include "e1000.h"
#include "pci.h"
#include "../kernel/idt.h"
#include "../kernel/types.h"
#include "../drivers/vga.h"

/* ── PCI IDs ─────────────────────────────────────────────────────── */
#define E1000_VENDOR  0x8086u
#define E1000_DEVICE  0x100Eu   /* 82540EM (QEMU default) */

/* ── MMIO register offsets ───────────────────────────────────────── */
#define E1000_CTRL   0x0000u
#define E1000_STATUS 0x0008u
#define E1000_EERD   0x0014u
#define E1000_ICR    0x00C0u
#define E1000_IMS    0x00D0u
#define E1000_IMC    0x00D8u
#define E1000_RCTL   0x0100u
#define E1000_TCTL   0x0400u
#define E1000_TIPG   0x0410u
#define E1000_RDBAL  0x2800u
#define E1000_RDBAH  0x2804u
#define E1000_RDLEN  0x2808u
#define E1000_RDH    0x2810u
#define E1000_RDT    0x2818u
#define E1000_TDBAL  0x3800u
#define E1000_TDBAH  0x3804u
#define E1000_TDLEN  0x3808u
#define E1000_TDH    0x3810u
#define E1000_TDT    0x3818u
#define E1000_MTA    0x5200u
#define E1000_RAL0   0x5400u
#define E1000_RAH0   0x5404u

/* ── Control register bits ───────────────────────────────────────── */
#define CTRL_SLU     (1u << 6)    /* Set Link Up */
#define CTRL_RST     (1u << 26)   /* Device Reset */

/* ── EERD bits ───────────────────────────────────────────────────── */
#define EERD_START   (1u << 0)
#define EERD_DONE    (1u << 4)

/* ── RCTL bits ───────────────────────────────────────────────────── */
#define RCTL_EN      (1u << 1)
#define RCTL_UPE     (1u << 3)    /* Unicast Promiscuous Enable  */
#define RCTL_MPE     (1u << 4)    /* Multicast Promiscuous Enable */
#define RCTL_BAM     (1u << 15)   /* Broadcast Accept */
#define RCTL_SECRC   (1u << 26)   /* Strip Ethernet CRC */

/* ── STATUS bits ─────────────────────────────────────────────────── */
#define STATUS_LU    (1u << 1)    /* Link Up */

/* ── TCTL bits ───────────────────────────────────────────────────── */
#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)    /* Pad Short Packets */

/* ── TX descriptor command bits ─────────────────────────────────── */
#define TDCMD_EOP    0x01u        /* End Of Packet */
#define TDCMD_IFCS   0x02u        /* Insert FCS (CRC) */
#define TDCMD_RS     0x08u        /* Report Status → set DD when done */

/* ── Descriptor done status bit ─────────────────────────────────── */
#define DESC_DD      0x01u

/* ── Ring and buffer sizes ───────────────────────────────────────── */
#define NUM_TX_DESC  8
#define NUM_RX_DESC  8
#define BUF_SIZE     2048

/* ── TX descriptor ───────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;       /* checksum offset */
    uint8_t  cmd;       /* command bits */
    uint8_t  sta;       /* status — hardware sets DD here */
    uint8_t  css;       /* checksum start */
    uint16_t special;
} tx_desc_t;

/* ── RX descriptor ───────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;    /* bit 0 = DD, bit 1 = EOP */
    uint8_t  errors;
    uint16_t special;
} rx_desc_t;

/* ── Static descriptor rings (must be 16-byte aligned) ──────────── */
static tx_desc_t tx_ring[NUM_TX_DESC] __attribute__((aligned(16)));
static rx_desc_t rx_ring[NUM_RX_DESC] __attribute__((aligned(16)));

/* ── Static packet buffers (2 KB each, 4K aligned) ──────────────── */
static uint8_t tx_buf[NUM_TX_DESC][BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t rx_buf[NUM_RX_DESC][BUF_SIZE] __attribute__((aligned(4096)));

/* ── Driver state ────────────────────────────────────────────────── */
static volatile uint8_t *g_mmio  = (volatile uint8_t *)0;
static uint8_t           g_mac[6];
static int               g_ready  = 0;
static uint32_t          g_tx_tail = 0;
static uint32_t          g_rx_head = 0;

/* ── MMIO helpers ────────────────────────────────────────────────── */
static inline uint32_t e1000_rd(uint32_t reg)
{
    return *(volatile uint32_t *)(g_mmio + reg);
}
static inline void e1000_wr(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(g_mmio + reg) = val;
}

/* ── EEPROM word read ────────────────────────────────────────────── */
static uint16_t eeprom_read(uint8_t addr)
{
    e1000_wr(E1000_EERD, ((uint32_t)addr << 8) | EERD_START);
    uint32_t v;
    do { v = e1000_rd(E1000_EERD); } while (!(v & EERD_DONE));
    return (uint16_t)(v >> 16);
}

/* ── IRQ handler — clears ICR; RX is polled via e1000_recv() ─────── */
static void e1000_irq_handler(void)
{
    e1000_rd(E1000_ICR);   /* read-to-clear */
}

/* ── e1000_init ──────────────────────────────────────────────────── */
int e1000_init(void)
{
    const pci_device_t *dev = pci_find_device(E1000_VENDOR, E1000_DEVICE);
    if (!dev) return -1;

    /* BAR0: 32-bit memory BAR — mask off the type/prefetch flag bits */
    uint32_t bar0 = pci_read32(dev->bus, dev->dev, dev->fn, 0x10) & ~0xFu;
    if (!bar0) return -1;
    g_mmio = (volatile uint8_t *)(uint64_t)bar0;

    /* Enable PCI memory space + bus mastering (bits 1 and 2) */
    uint32_t cmd = pci_read32(dev->bus, dev->dev, dev->fn, 0x04);
    pci_write32(dev->bus, dev->dev, dev->fn, 0x04, cmd | 0x06u);

    /* Hardware reset — poll until RST self-clears (hardware re-reads EEPROM) */
    e1000_wr(E1000_CTRL, e1000_rd(E1000_CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 2000000 && (e1000_rd(E1000_CTRL) & CTRL_RST); i++);

    /* Disable all interrupts before touching any other registers */
    e1000_wr(E1000_IMC, 0xFFFFFFFFu);
    e1000_rd(E1000_ICR);   /* clear any pending */

    /* Read MAC from RAL0/RAH0 — hardware auto-loaded these from EEPROM on reset.
       This avoids any EEPROM read byte-order bugs; we just trust the hardware. */
    {
        uint32_t ral0 = e1000_rd(E1000_RAL0);
        uint32_t rah0 = e1000_rd(E1000_RAH0);
        g_mac[0] = (uint8_t)( ral0        & 0xFF);
        g_mac[1] = (uint8_t)((ral0 >>  8) & 0xFF);
        g_mac[2] = (uint8_t)((ral0 >> 16) & 0xFF);
        g_mac[3] = (uint8_t)((ral0 >> 24) & 0xFF);
        g_mac[4] = (uint8_t)( rah0        & 0xFF);
        g_mac[5] = (uint8_t)((rah0 >>  8) & 0xFF);
        /* Make sure Address Valid bit is set */
        if (!(rah0 & (1u << 31)))
            e1000_wr(E1000_RAH0, rah0 | (1u << 31));
    }

    /* Zero the 128-entry multicast table */
    for (int i = 0; i < 128; i++) e1000_wr(E1000_MTA + (uint32_t)i * 4, 0);

    /* ── TX ring ─────────────────────────────────────────────────── */
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_ring[i].addr    = (uint64_t)tx_buf[i];
        tx_ring[i].length  = 0;
        tx_ring[i].sta     = DESC_DD;   /* pre-mark as done → usable */
        tx_ring[i].cmd     = 0;
        tx_ring[i].cso     = 0;
        tx_ring[i].css     = 0;
        tx_ring[i].special = 0;
    }
    uintptr_t tdba = (uintptr_t)tx_ring;
    e1000_wr(E1000_TDBAL, (uint32_t)(tdba & 0xFFFFFFFF));
    e1000_wr(E1000_TDBAH, (uint32_t)(tdba >> 32));
    e1000_wr(E1000_TDLEN, NUM_TX_DESC * 16u);
    e1000_wr(E1000_TDH, 0);
    e1000_wr(E1000_TDT, 0);
    g_tx_tail = 0;

    /* ── RX ring ─────────────────────────────────────────────────── */
    for (int i = 0; i < NUM_RX_DESC; i++) {
        rx_ring[i].addr    = (uint64_t)(uintptr_t)rx_buf[i]; // No (uint32_t) cast
        rx_ring[i].status  = 0;
        rx_ring[i].length  = 0;
    }
    uintptr_t rdba = (uintptr_t)rx_ring;
    e1000_wr(E1000_RDBAL, (uint32_t)(rdba & 0xFFFFFFFFu));
    e1000_wr(E1000_RDBAH, (uint32_t)(rdba >> 32));
    e1000_wr(E1000_RDLEN, NUM_RX_DESC * 16u);
    e1000_wr(E1000_RDH, 0);
    e1000_wr(E1000_RDT, NUM_RX_DESC - 1);
    g_rx_head = 0;

    /* Set link up and wait for STATUS.LU to confirm */
    e1000_wr(E1000_CTRL, e1000_rd(E1000_CTRL) | CTRL_SLU);
    for (volatile int i = 0; i < 2000000 && !(e1000_rd(E1000_STATUS) & STATUS_LU); i++);

    /* Inter-packet gap — standard value for 82540EM */
    e1000_wr(E1000_TIPG, 0x00702008u);

    /* TX control: enable, pad short packets, CT=0x0F, COLD=0x40 */
    e1000_wr(E1000_TCTL,
             TCTL_EN | TCTL_PSP |
             (0x0Fu <<  4) |   /* CT  — collision threshold */
             (0x40u << 12));   /* COLD — collision distance  */

    /* RX control: enable, promiscuous (UPE+MPE), broadcast, strip CRC.
       UPE/MPE ensure we receive SLIRP's ARP replies regardless of
       whether the NIC's MAC filter is programmed identically to what
       SLIRP expects. Without these, unicast replies get silently dropped. */
    e1000_wr(E1000_RCTL,
             RCTL_EN | RCTL_UPE | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    /* Install IRQ handler on the line reported in PCI INT_LINE */
    uint8_t irq_line = (uint8_t)(pci_read32(dev->bus, dev->dev, dev->fn, 0x3Cu) & 0xFFu);
    if (irq_line < 16) irq_install(irq_line, e1000_irq_handler);

    /* Enable receive-descriptor writeback interrupt */
    e1000_wr(E1000_IMS, 0x04u);   /* RXT0 */

    g_ready = 1;
    return 0;
}

/* ── Diagnostic register read (for shell commands) ───────────────── */
uint32_t e1000_reg(uint32_t off) { return g_ready ? e1000_rd(off) : 0; }

/* ── Link status ─────────────────────────────────────────────────── */
int e1000_link_up(void) { return g_ready && (e1000_rd(E1000_STATUS) & STATUS_LU) ? 1 : 0; }

/* ── e1000_send ──────────────────────────────────────────────────── */
int e1000_send(const void *data, uint16_t len)
{
    if (!g_ready || len > BUF_SIZE) return -1;

    /* Wait for current slot to become free (hardware sets DD when done) */
    while (!(tx_ring[g_tx_tail].sta & DESC_DD))
        __asm__ volatile("pause");

    /* Copy frame into the TX buffer for this slot */
    const uint8_t *src = (const uint8_t *)data;
    uint8_t       *dst = tx_buf[g_tx_tail];
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    /* Arm the descriptor */
    tx_ring[g_tx_tail].length  = len;
    tx_ring[g_tx_tail].cmd     = TDCMD_EOP | TDCMD_IFCS | TDCMD_RS;
    tx_ring[g_tx_tail].sta     = 0;        /* clear DD — hardware will set it */
    tx_ring[g_tx_tail].cso     = 0;
    tx_ring[g_tx_tail].css     = 0;
    tx_ring[g_tx_tail].special = 0;

    /* Advance tail — writing TDT kicks hardware to start transmitting */
    g_tx_tail = (g_tx_tail + 1u) % NUM_TX_DESC;
    e1000_wr(E1000_TDT, g_tx_tail);

    return 0;
}

/* ── e1000_recv ──────────────────────────────────────────────────── */
int e1000_recv(void *buf, uint16_t *len_out)
{
    if (!g_ready) return -1;

    rx_desc_t *desc = &rx_ring[g_rx_head];
    if (!(desc->status & DESC_DD)) return -1;   /* no frame ready */

    uint16_t len = desc->length;
    if (len > BUF_SIZE) len = (uint16_t)BUF_SIZE;

    uint8_t *dst = (uint8_t *)buf;
    uint8_t *src = rx_buf[g_rx_head];
    for (uint16_t i = 0; i < len; i++) dst[i] = src[i];

    *len_out = len;

    /* Return descriptor to hardware */
    desc->status  = 0;
    desc->length  = 0;
    desc->errors  = 0;
    e1000_wr(E1000_RDT, g_rx_head);
    g_rx_head = (g_rx_head + 1u) % NUM_RX_DESC;

    return 0;
}

/* ── e1000_mac ───────────────────────────────────────────────────── */
void e1000_mac(uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) mac[i] = g_mac[i];
}

/* ── e1000_ready ─────────────────────────────────────────────────── */
int e1000_ready(void) { return g_ready; }
