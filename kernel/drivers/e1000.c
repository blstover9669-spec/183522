/*
 * e1000.c - Intel PRO/1000 (8254x, "e1000") network card driver.
 *
 * VirtualBox's default adapters (PRO/1000 MT Desktop 82540EM, T Server 82543GC,
 * MT Server 82545EM) and QEMU's -nic ...,model=e1000 (82540EM) are this family.
 *
 * Legacy descriptors, one 256-entry receive ring with 2 KiB buffers and a
 * 64-entry transmit ring. The card's interrupts stay masked: the network stack
 * polls the receive ring from its 1 ms main-loop timer (netcore.c), which avoids
 * IRQ sharing / routing differences between VirtualBox and QEMU altogether.
 * Memory is identity mapped, so heap addresses are the physical addresses the
 * card DMAs to.
 */
#include "kernel.h"
#include "net/net_internal.h"

#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014
#define REG_ICR     0x00C0
#define REG_IMS     0x00D0
#define REG_IMC     0x00D8
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_RDTR    0x2820
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200
#define REG_RAL0    0x5400
#define REG_RAH0    0x5404

#define CTRL_FD      (1u << 0)
#define CTRL_LRST    (1u << 3)
#define CTRL_ASDE    (1u << 5)
#define CTRL_SLU     (1u << 6)
#define CTRL_RST     (1u << 26)
#define CTRL_VME     (1u << 30)
#define CTRL_PHY_RST (1u << 31)

#define STATUS_LU    (1u << 1)

#define RCTL_EN      (1u << 1)
#define RCTL_BAM     (1u << 15)
#define RCTL_SECRC   (1u << 26)

#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)

#define RX_COUNT 256
#define TX_COUNT 64
#define BUF_SIZE 2048

typedef struct PACKED {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} rx_desc_t;

typedef struct PACKED {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} tx_desc_t;

#define RXS_DD  0x01
#define RXS_EOP 0x02
#define TXC_EOP  0x01
#define TXC_IFCS 0x02
#define TXC_RS   0x08
#define TXS_DD   0x01

static volatile uint8_t *mmio;
static volatile rx_desc_t *rx_ring;
static volatile tx_desc_t *tx_ring;
static uint8_t *rx_bufs, *tx_bufs;
static uint32_t rx_next, tx_next;
static netdev_t dev;

static inline uint32_t rd(uint32_t reg) { return *(volatile uint32_t *)(mmio + reg); }
static inline void wr(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(mmio + reg) = v; }

static const struct { uint16_t id; const char *name; } models[] = {
    { 0x100E, "Intel PRO/1000 MT Desktop (82540EM)" },
    { 0x1004, "Intel PRO/1000 T Server (82543GC)" },
    { 0x100F, "Intel PRO/1000 MT Server (82545EM)" },
    { 0x1000, "Intel PRO/1000 (82542)" },
    { 0x1001, "Intel PRO/1000 F (82543GC)" },
    { 0x1008, "Intel PRO/1000 XT (82544EI)" },
    { 0x1009, "Intel PRO/1000 XF (82544EI)" },
    { 0x100C, "Intel PRO/1000 T (82544GC)" },
    { 0x100D, "Intel PRO/1000 (82544GC)" },
    { 0x1010, "Intel PRO/1000 MT Dual (82546EB)" },
    { 0x1011, "Intel PRO/1000 MF (82545EM)" },
    { 0x1012, "Intel PRO/1000 MF Dual (82546EB)" },
    { 0x1013, "Intel PRO/1000 (82541EI)" },
    { 0x1015, "Intel PRO/1000 MT (82540EM LOM)" },
    { 0x1016, "Intel PRO/1000 MT (82540EP LOM)" },
    { 0x1017, "Intel PRO/1000 MT (82540EP)" },
    { 0x1018, "Intel PRO/1000 MT (82541EI)" },
    { 0x1019, "Intel PRO/1000 CT (82547EI)" },
    { 0x101A, "Intel PRO/1000 (82547EI)" },
    { 0x101D, "Intel PRO/1000 MT Quad (82546EB)" },
    { 0x101E, "Intel PRO/1000 MT (82540EP LP)" },
    { 0x1026, "Intel PRO/1000 MT (82545GM)" },
    { 0x1027, "Intel PRO/1000 MF (82545GM)" },
    { 0x1028, "Intel PRO/1000 (82545GM)" },
    { 0x1075, "Intel PRO/1000 GT (82547GI)" },
    { 0x1076, "Intel PRO/1000 MT (82541GI)" },
    { 0x1077, "Intel PRO/1000 MT (82541GI)" },
    { 0x1078, "Intel PRO/1000 (82541ER)" },
    { 0x1079, "Intel PRO/1000 MT Dual (82546GB)" },
    { 0x107A, "Intel PRO/1000 MF Dual (82546GB)" },
    { 0x107B, "Intel PRO/1000 (82546GB)" },
    { 0x107C, "Intel PRO/1000 GT (82541PI)" },
    { 0x1099, "Intel PRO/1000 (82546GB Quad)" },
    { 0x10B5, "Intel PRO/1000 (82546GB Quad)" },
};

/* Read a 16-bit EEPROM word. The 82541/82547 family moved the START/DONE bits;
   try the classic layout first, then the newer one. */
static bool eeprom_read(uint16_t addr, uint16_t *out) {
    wr(REG_EERD, ((uint32_t)addr << 8) | 1u);
    for (int i = 0; i < 2000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1u << 4)) { *out = (uint16_t)(v >> 16); return true; }
        sys_udelay(5);
    }
    wr(REG_EERD, ((uint32_t)addr << 2) | 1u);
    for (int i = 0; i < 2000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1u << 1)) { *out = (uint16_t)(v >> 16); return true; }
        sys_udelay(5);
    }
    return false;
}

static bool e1000_link_up(void) { return mmio && (rd(REG_STATUS) & STATUS_LU); }

static bool e1000_send(const void *frame, uint32_t len) {
    if (!mmio || len > BUF_SIZE || len < 14) return false;
    volatile tx_desc_t *d = &tx_ring[tx_next];
    if (!(d->status & TXS_DD)) return false;            /* ring full */
    memcpy(tx_bufs + tx_next * BUF_SIZE, frame, len);
    d->addr = (uint32_t)(uintptr_t)(tx_bufs + tx_next * BUF_SIZE);
    d->length = (uint16_t)len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = TXC_EOP | TXC_IFCS | TXC_RS;
    tx_next = (tx_next + 1) % TX_COUNT;
    __asm__ volatile("" : : : "memory");
    wr(REG_TDT, tx_next);
    return true;
}

static int e1000_poll(net_rx_fn rx, int budget) {
    if (!mmio) return 0;
    int n = 0;
    while (n < budget) {
        volatile rx_desc_t *d = &rx_ring[rx_next];
        if (!(d->status & RXS_DD)) break;
        __asm__ volatile("" : : : "memory");
        uint16_t len = d->length;
        /* frames larger than one buffer (never with a 1500 MTU) and errored frames are dropped */
        if ((d->status & RXS_EOP) && !d->errors && len >= 14 && len <= BUF_SIZE)
            rx(rx_bufs + rx_next * BUF_SIZE, len);
        d->status = 0;
        uint32_t done = rx_next;
        rx_next = (rx_next + 1) % RX_COUNT;
        wr(REG_RDT, done);          /* give the buffer back to the card */
        n++;
    }
    if (n) (void)rd(REG_ICR);       /* clear latched causes (interrupts are masked anyway) */
    return n;
}

static void *alloc_aligned(uint32_t size, uint32_t align) {
    uint8_t *p = kcalloc(1, size + align);
    if (!p) return NULL;
    return (void *)(((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1));
}

static void e1000_init(void) {
    const pci_device_t *pd = NULL;
    const char *model = NULL;
    for (int i = 0; i < pci_device_count() && !pd; i++) {
        const pci_device_t *d = pci_get_device(i);
        if (d->vendor != 0x8086) continue;
        for (int k = 0; k < ARRAY_SIZE(models); k++)
            if (models[k].id == d->device) { pd = d; model = models[k].name; break; }
    }
    if (!pd) { klog("e1000: no Intel PRO/1000 network card found"); return; }
    if (pd->bar[0] & 1) { klog("e1000: BAR0 is not memory mapped (%08x)", pd->bar[0]); return; }
    pci_enable(pd);
    mmio = (volatile uint8_t *)(uintptr_t)(pd->bar[0] & ~0xFu);

    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    sys_udelay(1000);
    for (int i = 0; i < 1000 && (rd(REG_CTRL) & CTRL_RST); i++) sys_udelay(10);
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rd(REG_ICR);

    uint32_t ctrl = rd(REG_CTRL);
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_VME);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    wr(REG_CTRL, ctrl);

    /* MAC address: EEPROM words 0-2, else the receive address registers */
    uint16_t w[3];
    bool ee = eeprom_read(0, &w[0]) && eeprom_read(1, &w[1]) && eeprom_read(2, &w[2]);
    if (ee && (w[0] | w[1] | w[2]) && !(w[0] == 0xFFFF && w[1] == 0xFFFF && w[2] == 0xFFFF)) {
        for (int i = 0; i < 3; i++) { dev.mac[i * 2] = (uint8_t)w[i]; dev.mac[i * 2 + 1] = (uint8_t)(w[i] >> 8); }
    } else {
        uint32_t lo = rd(REG_RAL0), hi = rd(REG_RAH0);
        for (int i = 0; i < 4; i++) dev.mac[i] = (uint8_t)(lo >> (i * 8));
        dev.mac[4] = (uint8_t)hi;
        dev.mac[5] = (uint8_t)(hi >> 8);
    }
    wr(REG_RAL0, dev.mac[0] | (dev.mac[1] << 8) | (dev.mac[2] << 16) | ((uint32_t)dev.mac[3] << 24));
    wr(REG_RAH0, dev.mac[4] | (dev.mac[5] << 8) | (1u << 31));
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    rx_ring = alloc_aligned(sizeof(rx_desc_t) * RX_COUNT, 128);
    tx_ring = alloc_aligned(sizeof(tx_desc_t) * TX_COUNT, 128);
    rx_bufs = alloc_aligned(BUF_SIZE * RX_COUNT, 16);
    tx_bufs = alloc_aligned(BUF_SIZE * TX_COUNT, 16);
    if (!rx_ring || !tx_ring || !rx_bufs || !tx_bufs) { klog("e1000: out of memory"); mmio = NULL; return; }

    for (int i = 0; i < RX_COUNT; i++) {
        rx_ring[i].addr = (uint32_t)(uintptr_t)(rx_bufs + i * BUF_SIZE);
        rx_ring[i].status = 0;
    }
    for (int i = 0; i < TX_COUNT; i++) {
        tx_ring[i].addr = 0;
        tx_ring[i].cmd = 0;
        tx_ring[i].status = TXS_DD;
    }
    rx_next = tx_next = 0;

    wr(REG_RDBAL, (uint32_t)(uintptr_t)rx_ring);
    wr(REG_RDBAH, 0);
    wr(REG_RDLEN, sizeof(rx_desc_t) * RX_COUNT);
    wr(REG_RDH, 0);
    wr(REG_RDT, RX_COUNT - 1);
    wr(REG_RDTR, 0);
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);      /* 2048-byte buffers, broadcast on */

    wr(REG_TDBAL, (uint32_t)(uintptr_t)tx_ring);
    wr(REG_TDBAH, 0);
    wr(REG_TDLEN, sizeof(tx_desc_t) * TX_COUNT);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TIPG, 0x0060200A);
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));

    strlcpy(dev.name, model, sizeof dev.name);
    dev.link_up = e1000_link_up;
    dev.send = e1000_send;
    dev.poll = e1000_poll;
    klog("e1000: %s at %02x:%02x.%u, mmio %08x, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s",
         model, pd->bus, pd->slot, pd->func, (uint32_t)(uintptr_t)mmio,
         dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5],
         e1000_link_up() ? "up" : "down");
    net_register_device(&dev);
}
REGISTER_INIT(INIT_DRIVER, e1000_init);
