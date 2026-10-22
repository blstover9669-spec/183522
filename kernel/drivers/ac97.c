/*
 * ac97.c - Intel ICH AC'97 audio controller (82801AA "ICH AC97" as emulated by
 * VirtualBox and QEMU's -device AC97, and the later ICH variants with the same
 * native I/O interface).
 *
 *   BAR0 = NAM  (Native Audio Mixer: the codec's registers, 16-bit)
 *   BAR1 = NABM (Native Audio Bus Master: DMA engines, global control/status)
 *
 * PCM out uses the bus master's buffer descriptor list: 32 descriptors, each
 * pointing to its own buffer of BUF_FRAMES stereo 16-bit frames at 48 kHz. The
 * buffers live in the kernel's .bss, which is identity mapped (virtual address =
 * physical address) and physically contiguous, so the controller can read them.
 *
 * No device interrupt is used: audio.c calls ac97_pump() every millisecond from the
 * PIT interrupt. It reads the current descriptor index (CIV) and mixes new buffers
 * until `lead` buffers are queued ahead of the one playing, then moves the last
 * valid index (LVI) forward. If the controller ever ran dry (it halts at LVI, status
 * DCH), writing a new LVI restarts it and the lead grows a little.
 */
#include "audio_internal.h"

/* mixer (NAM) registers */
#define NAM_RESET       0x00
#define NAM_MASTER      0x02
#define NAM_HEADPHONE   0x04
#define NAM_MONO        0x06
#define NAM_PCM_OUT     0x18
#define NAM_POWERDOWN   0x26
#define NAM_EXT_ID      0x28
#define NAM_EXT_CTRL    0x2A
#define NAM_FRONT_RATE  0x2C
#define NAM_VENDOR1     0x7C
#define NAM_VENDOR2     0x7E

/* bus master (NABM) registers: PCM out box at 0x10 */
#define PO_BDBAR        0x10
#define PO_CIV          0x14
#define PO_LVI          0x15
#define PO_SR           0x16
#define PO_PICB         0x18
#define PO_PIV          0x1A
#define PO_CR           0x1B
#define GLOB_CNT        0x2C
#define GLOB_STA        0x30

#define CR_RPBM         0x01    /* run */
#define CR_RR           0x02    /* reset the box's registers */
#define SR_DCH          0x01    /* DMA controller halted */
#define SR_CLEAR        0x1C    /* LVBCI | BCIS | FIFOE (write 1 to clear) */

#define GC_COLD         0x02    /* AC'97 cold reset de-asserted */
#define GC_WARM         0x04    /* warm reset (self-clearing) */
#define GC_PCM_MASK     0x00F00000u
#define GS_CODEC_READY  0x10000300u   /* primary / secondary / tertiary codec ready */

#define BD_BUP          0x4000  /* after the last valid buffer, play silence */

#define NBUF            32
#define BUF_FRAMES      256     /* 5.3 ms at 48 kHz */
#define LEAD_INIT       8       /* buffers queued ahead: ~43 ms */
#define LEAD_MAX        26
#define MAX_FILLS       4       /* buffers mixed per tick at most (also the prefill at start;
                                   the next ticks top it up to `lead`) */

typedef struct PACKED bdesc {
    uint32_t addr;
    uint16_t samples;           /* 16-bit samples (2 per stereo frame) */
    uint16_t flags;
} bdesc_t;

static bdesc_t bdl[NBUF] __attribute__((aligned(32)));
static int16_t bufs[NBUF][BUF_FRAMES * 2] __attribute__((aligned(4096)));
static int16_t mono[BUF_FRAMES];
static uint16_t nam, nabm;
static int next_fill, last_civ, lead = LEAD_INIT;
static bool halted;
static sound_card_t ac97_card;

static const uint16_t ac97_ids[][2] = {
    { 0x8086, 0x2415 },   /* 82801AA (ICH): VirtualBox, QEMU */
    { 0x8086, 0x2425 },   /* 82801AB (ICH0) */
    { 0x8086, 0x2445 },   /* 82801BA (ICH2) */
    { 0x8086, 0x2485 },   /* 82801CA (ICH3) */
    { 0x8086, 0x24C5 },   /* 82801DB (ICH4) */
    { 0x8086, 0x24D5 },   /* 82801EB (ICH5) */
    { 0x8086, 0x266E },   /* 82801FB (ICH6) */
    { 0x8086, 0x27DE },   /* 82801GB (ICH7) */
    { 0x8086, 0x7195 },   /* 440MX */
};

static void fill(int i) {
    audio_render(mono, BUF_FRAMES);
    int16_t *b = bufs[i];
    for (int k = 0; k < BUF_FRAMES; k++) b[2 * k] = b[2 * k + 1] = mono[k];
}

/* stop the PCM out DMA engine and reset its registers (bounded busy waits) */
static void po_reset(void) {
    outb(nabm + PO_CR, 0);
    for (int i = 0; i < 1000 && !(inw(nabm + PO_SR) & SR_DCH); i++) io_wait();
    outb(nabm + PO_CR, CR_RR);
    for (int i = 0; i < 1000 && (inb(nabm + PO_CR) & CR_RR); i++) io_wait();
    outw(nabm + PO_SR, SR_CLEAR);
}

static void ac97_start(void) {
    po_reset();
    outl(nabm + PO_BDBAR, (uint32_t)(uintptr_t)bdl);
    int pre = MIN(lead, MAX_FILLS);
    for (int i = 0; i < pre; i++) fill(i);
    next_fill = pre;
    last_civ = 0;
    halted = false;
    outb(nabm + PO_LVI, (uint8_t)((next_fill - 1) & (NBUF - 1)));
    outb(nabm + PO_CR, CR_RPBM);
}

static void ac97_stop(void) { po_reset(); }

static void ac97_pump(void) {
    uint16_t sr = inw(nabm + PO_SR);
    if (sr & SR_CLEAR) outw(nabm + PO_SR, sr & SR_CLEAR);
    if (sr & SR_DCH) {
        /* ran dry and stopped at LVI: the LVI write below restarts it */
        if (!halted) {
            audio_note_underrun();
            if (lead < LEAD_MAX) lead += 2;
        }
        halted = true;
    } else {
        halted = false;
    }
    int civ = inb(nabm + PO_CIV) & (NBUF - 1);
    if (civ != last_civ) {
        last_civ = civ;
        audio_note_progress();
    }
    int queued = (next_fill - civ) & (NBUF - 1);
    int fills = 0;
    while (queued < lead && fills < MAX_FILLS) {
        fill(next_fill);
        next_fill = (next_fill + 1) & (NBUF - 1);
        queued++;
        fills++;
    }
    if (fills) outb(nabm + PO_LVI, (uint8_t)((next_fill - 1) & (NBUF - 1)));
}

static bool wait_glob(uint32_t mask, bool set, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        bool on = (inl(nabm + GLOB_STA) & mask) != 0;
        if (on == set) return true;
        sys_udelay(100);
    }
    return false;
}

const sound_card_t *ac97_probe(void) {
    const pci_device_t *d = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(ac97_ids) && !d; i++) d = pci_find_device(ac97_ids[i][0], ac97_ids[i][1]);
    if (!d) return NULL;
    if (!(d->bar[0] & 1) || !(d->bar[1] & 1)) {
        klog("ac97: %04x:%04x has no I/O BARs (%08x %08x) - not used", d->vendor, d->device, d->bar[0], d->bar[1]);
        return NULL;
    }
    nam = (uint16_t)(d->bar[0] & 0xFFFC);
    nabm = (uint16_t)(d->bar[1] & 0xFFFC);
    pci_enable(d);

    /* finish a cold reset (or do a warm one), 2-channel 16-bit PCM, no GPI interrupt */
    uint32_t cnt = inl(nabm + GLOB_CNT);
    cnt &= ~(GC_PCM_MASK | 0x01u);
    cnt |= (cnt & GC_COLD) ? GC_WARM : GC_COLD;
    outl(nabm + GLOB_CNT, cnt);
    for (int i = 0; i < 1000 && (inl(nabm + GLOB_CNT) & GC_WARM); i++) sys_udelay(100);
    if (!wait_glob(GS_CODEC_READY, true, 400)) {
        /* still not ready: pulse a full cold reset */
        outl(nabm + GLOB_CNT, cnt & ~(GC_COLD | GC_WARM));
        sys_udelay(100);
        outl(nabm + GLOB_CNT, (cnt & ~GC_WARM) | GC_COLD);
        if (!wait_glob(GS_CODEC_READY, true, 600)) klog("ac97: codec did not report ready (status %08x)", inl(nabm + GLOB_STA));
    }

    /* codec register reset, wait for the DAC / analog / Vref sections */
    outw(nam + NAM_RESET, 0);
    for (int i = 0; i < 200 && (inw(nam + NAM_POWERDOWN) & 0x0F) != 0x0F; i++) sys_udelay(1000);

    /* unmute: master and headphone at 0 dB attenuation, PCM out at 0 dB gain */
    outw(nam + NAM_MASTER, 0x0000);
    outw(nam + NAM_HEADPHONE, 0x0000);
    outw(nam + NAM_MONO, 0x0000);
    outw(nam + NAM_PCM_OUT, 0x0808);

    /* 48 kHz is the AC'97 default; with variable rate audio set it explicitly */
    int r = 48000;
    uint16_t ext = inw(nam + NAM_EXT_ID);
    if (ext & 1) {
        outw(nam + NAM_EXT_CTRL, (uint16_t)(inw(nam + NAM_EXT_CTRL) | 1));
        outw(nam + NAM_FRONT_RATE, 48000);
        int got = inw(nam + NAM_FRONT_RATE);
        if (got >= 8000 && got <= 48000) r = got;
    }

    po_reset();
    for (int i = 0; i < NBUF; i++) {
        memset(bufs[i], 0, sizeof bufs[i]);
        bdl[i].addr = (uint32_t)(uintptr_t)bufs[i];
        bdl[i].samples = BUF_FRAMES * 2;
        bdl[i].flags = BD_BUP;
    }
    outl(nabm + PO_BDBAR, (uint32_t)(uintptr_t)bdl);

    klog("ac97: %04x:%04x at %02x:%02x.%u, mixer %x, bus master %x, codec %04x%04x, master %04x pcm %04x, %d Hz%s",
         d->vendor, d->device, d->bus, d->slot, d->func, nam, nabm, inw(nam + NAM_VENDOR1), inw(nam + NAM_VENDOR2),
         inw(nam + NAM_MASTER), inw(nam + NAM_PCM_OUT), r, (ext & 1) ? " (VRA)" : "");

    ac97_card.name = "Intel ICH AC'97";
    ac97_card.short_name = "AC'97";
    ac97_card.rate = r;
    ac97_card.start = ac97_start;
    ac97_card.stop = ac97_stop;
    ac97_card.pump = ac97_pump;
    return &ac97_card;
}
