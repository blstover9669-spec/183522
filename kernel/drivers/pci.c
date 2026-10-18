/* pci.c - PCI configuration space access and bus enumeration. */
#include "kernel.h"

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC
#define MAX_PCI 64

static pci_device_t devices[MAX_PCI];
static int ndevices;

static inline uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (off & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    outl(PCI_ADDR, pci_addr(bus, slot, func, off));
    return inl(PCI_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint16_t)(pci_read32(bus, slot, func, off) >> ((off & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    return (uint8_t)(pci_read32(bus, slot, func, off) >> ((off & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v) {
    outl(PCI_ADDR, pci_addr(bus, slot, func, off));
    outl(PCI_DATA, v);
}

void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v) {
    uint32_t old = pci_read32(bus, slot, func, off);
    int shift = (off & 2) * 8;
    old = (old & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
    pci_write32(bus, slot, func, off, old);
}

static void probe(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t id = pci_read32(bus, slot, func, 0);
    if ((id & 0xFFFF) == 0xFFFF || ndevices >= MAX_PCI) return;
    pci_device_t *d = &devices[ndevices++];
    d->bus = bus; d->slot = slot; d->func = func;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    uint32_t cls = pci_read32(bus, slot, func, 8);
    d->revision = cls & 0xFF;
    d->prog_if = (cls >> 8) & 0xFF;
    d->subclass = (cls >> 16) & 0xFF;
    d->class_code = (cls >> 24) & 0xFF;
    d->irq_line = pci_read8(bus, slot, func, 0x3C);
    for (int i = 0; i < 6; i++) d->bar[i] = pci_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
}

void pci_init(void) {
    ndevices = 0;
    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint8_t hdr = pci_read8((uint8_t)bus, (uint8_t)slot, 0, 0x0E);
            int nf = (hdr & 0x80) ? 8 : 1;
            for (int f = 0; f < nf; f++) probe((uint8_t)bus, (uint8_t)slot, (uint8_t)f);
        }
    }
    for (int i = 0; i < ndevices; i++) {
        pci_device_t *d = &devices[i];
        klog("pci %02x:%02x.%u %04x:%04x class %02x.%02x irq %u", d->bus, d->slot, d->func,
             d->vendor, d->device, d->class_code, d->subclass, d->irq_line);
    }
}

int pci_device_count(void) { return ndevices; }
const pci_device_t *pci_get_device(int i) { return (i >= 0 && i < ndevices) ? &devices[i] : NULL; }

const pci_device_t *pci_find_class(uint8_t cls, uint8_t sub) {
    for (int i = 0; i < ndevices; i++)
        if (devices[i].class_code == cls && devices[i].subclass == sub) return &devices[i];
    return NULL;
}

const pci_device_t *pci_find_device(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < ndevices; i++)
        if (devices[i].vendor == vendor && devices[i].device == device) return &devices[i];
    return NULL;
}

void pci_enable(const pci_device_t *d) {
    uint16_t cmd = pci_read16(d->bus, d->slot, d->func, 4);
    cmd |= 0x7;   /* I/O space, memory space, bus master */
    pci_write16(d->bus, d->slot, d->func, 4, cmd);
}
