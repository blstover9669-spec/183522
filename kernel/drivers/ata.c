/*
 * ata.c - ATA hard disk driver (PIO, polled) for the legacy IDE channels.
 *
 *   primary   0x1F0-0x1F7, control 0x3F6
 *   secondary 0x170-0x177, control 0x376
 *
 * Probes master + slave on both channels with IDENTIFY DEVICE, skips ATAPI
 * (CD/DVD) and SATA signatures, reads the model string and the LBA28 / LBA48
 * capacity. Reads and writes use READ/WRITE SECTORS (EXT) with BSY/DRQ/ERR
 * polling and timeouts; interrupts are disabled on the device (nIEN), so no
 * IRQ 14/15 handler is needed. Works with QEMU's PIIX IDE and VirtualBox's
 * PIIX3/PIIX4 IDE controllers (both run in legacy/compatibility mode).
 *
 * Empty positions: a channel without a controller floats to 0xFF; VirtualBox
 * returns 0x7F (ATA_EMPTY_BUS_DATA: D7 pulled down) from every register of an
 * empty channel and of a selected-but-absent device 0; a missing device 1
 * behind an ATAPI device 0 reads 0. Status 0x7F/0xFF (impossible for a real
 * drive: BSY clear with DRQ, DF and ERR all set) and LBA registers reading
 * 0x7F/0xFF count as "no device", so a VM with only the CD (or nothing)
 * reports no hard disks cleanly.
 */
#include "kernel.h"
#include <nova/disk.h>

#define REG_DATA     0
#define REG_ERROR    1
#define REG_FEATURES 1
#define REG_COUNT    2
#define REG_LBA0     3
#define REG_LBA1     4
#define REG_LBA2     5
#define REG_DRIVE    6
#define REG_STATUS   7
#define REG_COMMAND  7

#define SR_BSY  0x80
#define SR_DRDY 0x40
#define SR_DF   0x20
#define SR_DRQ  0x08
#define SR_ERR  0x01

#define CMD_READ_PIO      0x20
#define CMD_READ_PIO_EXT  0x24
#define CMD_WRITE_PIO     0x30
#define CMD_WRITE_PIO_EXT 0x34
#define CMD_FLUSH         0xE7
#define CMD_FLUSH_EXT     0xEA
#define CMD_IDENTIFY      0xEC

#define CTL_NIEN 0x02
#define CTL_SRST 0x04

static const uint16_t io_base[2]  = { 0x1F0, 0x170 };
static const uint16_t ctl_base[2] = { 0x3F6, 0x376 };
static const char *const locations[4] = { "Primary master", "Primary slave", "Secondary master", "Secondary slave" };

static disk_info_t disks[DISK_MAX];
static int ndisks;
static int selected[2] = { -1, -1 };     /* last drive-select value per channel */

/* ---- timing ------------------------------------------------------------------------ */
/* Timeouts use the PIT tick counter when interrupts are on, else a spin count
   (each spin reads a port, roughly a microsecond on real hardware). */
typedef struct { uint32_t start, spins, limit_ms; bool ticks; } tmo_t;

static void tmo_start(tmo_t *t, uint32_t ms) {
    uint32_t f;
    __asm__ volatile("pushf; pop %0" : "=r"(f));
    t->ticks = (f & 0x200) != 0;
    t->start = g_ticks;
    t->spins = 0;
    t->limit_ms = ms;
}

static bool tmo_expired(tmo_t *t) {
    if (t->ticks) return g_ticks - t->start > t->limit_ms;
    return ++t->spins > t->limit_ms * 2000u;
}

/* Test option ataempty=MASK (bit 0 primary master, 1 primary slave, 2 secondary
   master, 3 secondary slave): those positions read 0x7F from every register while
   selected, like VirtualBox's empty bus, so the probe can be tested in QEMU. */
static unsigned fake_empty;

static inline uint8_t reg_in(int ch, uint16_t port) {
    if (fake_empty && selected[ch] >= 0 && (fake_empty >> (ch * 2 + ((selected[ch] >> 4) & 1)) & 1)) return 0x7F;
    return inb(port);
}
static inline uint8_t alt_status(int ch) { return reg_in(ch, ctl_base[ch]); }
static inline uint8_t status(int ch) { return reg_in(ch, (uint16_t)(io_base[ch] + REG_STATUS)); }

/* Floating / empty bus values (0xFF: nothing decodes the port; 0x7F: VirtualBox). */
static inline bool bus_float(uint8_t v) { return v == 0xFF || v == 0x7F; }

static void delay400(int ch) {
    for (int i = 0; i < 5; i++) (void)alt_status(ch);
}

/* Wait until BSY clears. Returns the final status, or -1 on timeout. */
static int wait_idle(int ch, uint32_t ms) {
    tmo_t t;
    tmo_start(&t, ms);
    for (;;) {
        uint8_t s = alt_status(ch);
        if (bus_float(s)) return -1;         /* floating / empty bus: nothing there */
        if (!(s & SR_BSY)) return s;
        if (tmo_expired(&t)) return -1;
    }
}

/* Wait for BSY clear and DRQ set. Returns DISK_OK, DISK_EIO or DISK_ETIMEOUT. */
static int wait_drq(int ch, uint32_t ms) {
    tmo_t t;
    tmo_start(&t, ms);
    for (;;) {
        uint8_t s = alt_status(ch);
        if (!(s & SR_BSY)) {
            if (s & (SR_ERR | SR_DF)) return DISK_EIO;
            if (s & SR_DRQ) return DISK_OK;
        }
        if (tmo_expired(&t)) return DISK_ETIMEOUT;
    }
}

static void select_drive(int ch, int value) {
    if (selected[ch] == value) return;
    outb((uint16_t)(io_base[ch] + REG_DRIVE), (uint8_t)value);
    delay400(ch);
    selected[ch] = value;
}

static void soft_reset(int ch) {
    outb(ctl_base[ch], CTL_SRST | CTL_NIEN);
    sys_udelay(10);
    outb(ctl_base[ch], CTL_NIEN);
    sys_udelay(2000);
    wait_idle(ch, 3000);
    selected[ch] = -1;
}

/* ---- identification ------------------------------------------------------------------ */
static void ata_string(const uint16_t *id, int first, int words, char *out) {
    int n = 0;
    for (int i = 0; i < words; i++) {
        out[n++] = (char)(id[first + i] >> 8);
        out[n++] = (char)(id[first + i] & 0xFF);
    }
    out[n] = 0;
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0)) out[--n] = 0;
    int s = 0;
    while (out[s] == ' ') s++;
    if (s) memmove(out, out + s, (size_t)(n - s + 1));
}

/* IDENTIFY DEVICE: ID_DISK fills *d; ID_NONE = nothing there; ID_SKIP = a device
   that is not a usable hard disk (logged here). */
enum { ID_DISK, ID_NONE, ID_SKIP };

static int identify(int ch, int slave, disk_info_t *d) {
    uint16_t base = io_base[ch];
    select_drive(ch, 0xA0 | (slave << 4));
    if (wait_idle(ch, 2000) < 0) return ID_NONE;    /* empty bus (0x7F/0xFF) or stuck BSY */
    outb((uint16_t)(base + REG_COUNT), 0);
    outb((uint16_t)(base + REG_LBA0), 0);
    outb((uint16_t)(base + REG_LBA1), 0);
    outb((uint16_t)(base + REG_LBA2), 0);
    outb((uint16_t)(base + REG_COMMAND), CMD_IDENTIFY);
    delay400(ch);
    uint8_t s = status(ch);
    if (s == 0 || bus_float(s)) return ID_NONE;     /* no device */
    if (wait_idle(ch, 1000) < 0) return ID_NONE;
    uint8_t l1 = reg_in(ch, (uint16_t)(base + REG_LBA1)), l2 = reg_in(ch, (uint16_t)(base + REG_LBA2));
    if (bus_float(l1) && bus_float(l2)) return ID_NONE;  /* empty bus, not a signature */
    if ((l1 == 0x14 && l2 == 0xEB) || (l1 == 0x69 && l2 == 0x96)) {
        klog("ata: %s: ATAPI device (CD/DVD), skipped", locations[ch * 2 + slave]);
        return ID_SKIP;
    }
    if (l1 || l2) {
        klog("ata: %s: unknown signature %02x/%02x, skipped", locations[ch * 2 + slave], l1, l2);
        return ID_SKIP;
    }
    int r = wait_drq(ch, 1000);
    if (r == DISK_ETIMEOUT) {
        klog("ata: %s: no answer to IDENTIFY, skipped", locations[ch * 2 + slave]);
        return ID_SKIP;
    }
    if (r != DISK_OK) return ID_NONE;               /* aborted: no ATA disk there */
    uint16_t id[256];
    insw((uint16_t)(base + REG_DATA), id, 256);

    memset(d, 0, sizeof *d);
    d->channel = ch;
    d->slave = slave;
    d->location = locations[ch * 2 + slave];
    ata_string(id, 27, 20, d->model);
    ata_string(id, 10, 10, d->serial);
    if (!d->model[0]) strlcpy(d->model, "ATA hard disk", sizeof d->model);
    if (!(id[49] & (1 << 9))) {
        klog("ata: %s: %s has no LBA support, skipped", d->location, d->model);
        return ID_SKIP;
    }
    uint32_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    d->sectors = lba28;
    if (id[83] & (1 << 10)) {
        uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) |
                         ((uint64_t)id[103] << 48);
        if (lba48) { d->lba48 = true; if (lba48 > d->sectors) d->sectors = lba48; }
    }
    if (!d->sectors) {
        klog("ata: %s: %s reports 0 sectors, skipped", d->location, d->model);
        return ID_SKIP;
    }
    return ID_DISK;
}

/* Status of the master and the slave position (0x7F/0xFF: nothing there). */
static void probe_status(int ch, uint8_t st[2]) {
    for (int sl = 0; sl < 2; sl++) {
        selected[ch] = -1;
        select_drive(ch, 0xA0 | (sl << 4));
        st[sl] = status(ch);
    }
}

int disk_rescan(void) {
    ndisks = 0;
    for (int ch = 0; ch < 2; ch++) {
        const char *name = ch ? "secondary" : "primary";
        uint8_t st[2];
        probe_status(ch, st);
        if (bus_float(st[0]) && bus_float(st[1])) {
            klog("ata: %s channel: no devices (empty bus, status %02x/%02x)", name, st[0], st[1]);
            continue;
        }
        outb(ctl_base[ch], CTL_NIEN);         /* polled mode: no IRQs from this channel */
        if ((!bus_float(st[0]) && (st[0] & SR_BSY)) || (!bus_float(st[1]) && (st[1] & SR_BSY))) {
            soft_reset(ch);
            probe_status(ch, st);
        }
        for (int sl = 0; sl < 2 && ndisks < DISK_MAX; sl++) {
            disk_info_t d;
            int r = bus_float(st[sl]) ? ID_NONE : identify(ch, sl, &d);
            if (r == ID_NONE) klog("ata: %s: no device", locations[ch * 2 + sl]);
            if (r != ID_DISK) continue;
            d.index = ndisks;
            disks[ndisks++] = d;
            char sz[24];
            disk_format_size(d.sectors * 512, sz, sizeof sz);
            klog("ata: disk %d: %s \"%s\" %s (%llu sectors, %s)", d.index, d.location, d.model, sz,
                 (unsigned long long)d.sectors, d.lba48 ? "LBA48" : "LBA28");
        }
    }
    if (!ndisks) klog("ata: no disk (no hard disks found)");
    return ndisks;
}

static void ata_init(void) {
    const char *fe = sys_cmdline_get("ataempty");
    if (fe) {
        fake_empty = (unsigned)strtol(fe, NULL, 0) & 15;
        klog("ata: test mode: positions %x read as an empty VirtualBox bus (0x7F)", fake_empty);
    }
    disk_rescan();
}
REGISTER_INIT(INIT_DRIVER, ata_init);

int disk_count(void) { return ndisks; }

const disk_info_t *disk_get(int index) {
    if (index < 0 || index >= ndisks) return NULL;
    return &disks[index];
}

/* ---- sector I/O ---------------------------------------------------------------------- */
static int rw_chunk(const disk_info_t *d, uint64_t lba, uint32_t count, void *buf, bool write) {
    int ch = d->channel;
    uint16_t base = io_base[ch];
    bool ext = d->lba48 && (lba + count > 0x0FFFFFFFull || count > 256);
    if (!ext && count > 256) return DISK_EINVAL;
    if (wait_idle(ch, 2000) < 0) return DISK_ETIMEOUT;
    if (ext) select_drive(ch, 0x40 | (d->slave << 4));
    else select_drive(ch, 0xE0 | (d->slave << 4) | (int)((lba >> 24) & 0x0F));
    if (wait_idle(ch, 2000) < 0) return DISK_ETIMEOUT;
    if (ext) {
        outb((uint16_t)(base + REG_COUNT), (uint8_t)(count >> 8));
        outb((uint16_t)(base + REG_LBA0), (uint8_t)(lba >> 24));
        outb((uint16_t)(base + REG_LBA1), (uint8_t)(lba >> 32));
        outb((uint16_t)(base + REG_LBA2), (uint8_t)(lba >> 40));
    }
    outb((uint16_t)(base + REG_COUNT), (uint8_t)count);      /* 0 = 256 (LBA28) / 65536 (LBA48) */
    outb((uint16_t)(base + REG_LBA0), (uint8_t)lba);
    outb((uint16_t)(base + REG_LBA1), (uint8_t)(lba >> 8));
    outb((uint16_t)(base + REG_LBA2), (uint8_t)(lba >> 16));
    outb((uint16_t)(base + REG_COMMAND),
         write ? (ext ? CMD_WRITE_PIO_EXT : CMD_WRITE_PIO) : (ext ? CMD_READ_PIO_EXT : CMD_READ_PIO));
    uint16_t *p = buf;
    for (uint32_t i = 0; i < count; i++) {
        delay400(ch);
        int r = wait_drq(ch, 5000);
        if (r) return r;
        if (write) outsw((uint16_t)(base + REG_DATA), p, 256);
        else insw((uint16_t)(base + REG_DATA), p, 256);
        p += 256;
    }
    if (write) {
        delay400(ch);
        int s = wait_idle(ch, 5000);
        if (s < 0) return DISK_ETIMEOUT;
        if (s & (SR_ERR | SR_DF)) return DISK_EIO;
    } else {
        (void)status(ch);    /* acknowledge */
    }
    return DISK_OK;
}

static int disk_rw(int disk, uint64_t lba, uint32_t count, void *buf, bool write) {
    const disk_info_t *d = disk_get(disk);
    if (!d || !buf) return DISK_EINVAL;
    if (count == 0) return DISK_OK;
    if (lba >= d->sectors || count > d->sectors - lba) return DISK_EINVAL;
    if (!d->lba48 && lba + count > 0x0FFFFFFFull) return DISK_EINVAL;
    uint8_t *p = buf;
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        int r = DISK_EIO;
        for (int attempt = 0; attempt < 3; attempt++) {
            r = rw_chunk(d, lba, n, p, write);
            if (r == DISK_OK) break;
            klog("ata: %s error %d at lba %llu (attempt %d, status %02x, error %02x)", write ? "write" : "read", r,
                 (unsigned long long)lba, attempt + 1, alt_status(d->channel),
                 inb((uint16_t)(io_base[d->channel] + REG_ERROR)));
            soft_reset(d->channel);
        }
        if (r) return r;
        lba += n;
        count -= n;
        p += n * 512;
    }
    return DISK_OK;
}

int disk_read(int disk, uint64_t lba, uint32_t count, void *buf) { return disk_rw(disk, lba, count, buf, false); }

int disk_write(int disk, uint64_t lba, uint32_t count, const void *buf) {
    return disk_rw(disk, lba, count, (void *)buf, true);
}

int disk_flush(int disk) {
    const disk_info_t *d = disk_get(disk);
    if (!d) return DISK_EINVAL;
    int ch = d->channel;
    if (wait_idle(ch, 2000) < 0) return DISK_ETIMEOUT;
    select_drive(ch, 0xE0 | (d->slave << 4));
    if (wait_idle(ch, 2000) < 0) return DISK_ETIMEOUT;
    outb((uint16_t)(io_base[ch] + REG_COMMAND), d->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
    delay400(ch);
    int s = wait_idle(ch, 30000);
    if (s < 0) return DISK_ETIMEOUT;
    return (s & (SR_ERR | SR_DF)) ? DISK_EIO : DISK_OK;
}

void disk_format_size(uint64_t bytes, char *out, int n) {
    const uint64_t KB = 1024, MB = KB * 1024, GB = MB * 1024;
    if (bytes >= GB) {
        uint64_t t = bytes * 10 / GB;
        if (t % 10) snprintf(out, (size_t)n, "%u.%u GB", (unsigned)(t / 10), (unsigned)(t % 10));
        else snprintf(out, (size_t)n, "%u GB", (unsigned)(t / 10));
    } else if (bytes >= MB) {
        uint64_t t = bytes * 10 / MB;
        if (t % 10 && t < 1000) snprintf(out, (size_t)n, "%u.%u MB", (unsigned)(t / 10), (unsigned)(t % 10));
        else snprintf(out, (size_t)n, "%u MB", (unsigned)(t / 10));
    } else if (bytes >= KB) {
        snprintf(out, (size_t)n, "%u KB", (unsigned)(bytes / KB));
    } else {
        snprintf(out, (size_t)n, "%u bytes", (unsigned)bytes);
    }
}

const char *disk_strerror(int err) {
    switch (err) {
    case DISK_OK: return "Success";
    case DISK_EINVAL: return "Invalid disk or sector range";
    case DISK_EIO: return "The disk reported an error";
    case DISK_ETIMEOUT: return "The disk did not respond";
    case DISK_ENOMEM: return "Not enough memory";
    case DISK_ENOSPC: return "Not enough space on the disk";
    case DISK_ECORRUPT: return "Data on the disk is damaged";
    case DISK_ENODEV: return "No such disk";
    case DISK_EBUSY: return "The disk is busy";
    default: return "Unknown disk error";
    }
}
