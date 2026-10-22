/*
 * sb16.c - Creative Sound Blaster 16 (VirtualBox "SoundBlaster 16", QEMU -device sb16):
 * the fallback sound card when there is no AC'97.
 *
 *   - DSP at 0x220 (or 0x240): reset, version 4.xx = SB16
 *   - IRQ and 16-bit DMA channel from the mixer's configuration registers 0x80 / 0x81
 *   - 16-bit signed mono at 44.1 kHz, auto-init DMA (8237 controller 2, channel 5
 *     normally) over a ring of RING samples in .bss (below 16 MiB, not crossing a
 *     128 KiB boundary: an ISA DMA requirement)
 *
 * Like the AC'97 driver there is no work in the device IRQ (it is only acknowledged):
 * audio.c calls sb16_pump() every millisecond; it reads the DMA controller's current
 * count to find the play position, clears what was just played (so if playback ever
 * overtakes us it plays silence, not old sound) and mixes new samples to stay `lead`
 * samples ahead.
 */
#include "audio_internal.h"

#define DSP_MIXER       0x04
#define DSP_MIXER_DATA  0x05
#define DSP_RESET       0x06
#define DSP_READ        0x0A
#define DSP_WRITE       0x0C      /* write data / command; bit 7 of a read = busy */
#define DSP_RSTATUS     0x0E      /* bit 7 = data available; read = ack 8-bit IRQ */
#define DSP_ACK16       0x0F      /* read = ack 16-bit IRQ */

#define RATE            44100
#define RING            8192      /* samples (16 KiB), ~186 ms */
#define BLOCK           (RING / 2)
#define CHUNK_S         256
#define LEAD_INIT       2048      /* ~46 ms */
#define LEAD_MAX        (RING / 2)

static int16_t sb_ring[RING] __attribute__((aligned(16384)));
static uint16_t base;
static int irq = -1, hdma = 5;
static int wpos, last_ppos, lead = LEAD_INIT;
static sound_card_t sb_card;

/* 8237 controller 2 ports for 16-bit channels 4..7 */
static const uint8_t dma_addr_port[4] = { 0xC0, 0xC4, 0xC8, 0xCC };
static const uint8_t dma_count_port[4] = { 0xC2, 0xC6, 0xCA, 0xCE };
static const uint8_t dma_page_port[4] = { 0x8F, 0x8B, 0x89, 0x8A };

static bool dsp_write(uint8_t v) {
    for (int i = 0; i < 10000; i++) {
        if (!(inb(base + DSP_WRITE) & 0x80)) {
            outb(base + DSP_WRITE, v);
            return true;
        }
    }
    return false;
}

static int dsp_read(void) {
    for (int i = 0; i < 10000; i++)
        if (inb(base + DSP_RSTATUS) & 0x80) return inb(base + DSP_READ);
    return -1;
}

static bool dsp_reset(uint16_t port) {
    outb(port + DSP_RESET, 1);
    audio_delay_us(5);
    outb(port + DSP_RESET, 0);
    for (int i = 0; i < 2000; i++) {
        if ((inb(port + DSP_RSTATUS) & 0x80) && inb(port + DSP_READ) == 0xAA) return true;
        io_wait();
    }
    return false;
}

static uint8_t mixer_read(uint8_t reg) {
    outb(base + DSP_MIXER, reg);
    return inb(base + DSP_MIXER_DATA);
}

static void mixer_write(uint8_t reg, uint8_t v) {
    outb(base + DSP_MIXER, reg);
    outb(base + DSP_MIXER_DATA, v);
}

static void sb_irq(regs_t *r) {
    (void)r;
    uint8_t st = mixer_read(0x82);
    if (st & 2) inb(base + DSP_ACK16);
    if (st & 1) inb(base + DSP_RSTATUS);
}

/* samples left in the current DMA pass (the 8237's count + 1), read consistently */
static int dma_remaining(void) {
    int c = hdma & 3, prev = -1;
    for (int tries = 0; tries < 4; tries++) {
        outb(0xD8, 0);
        int lo = inb(dma_count_port[c]);
        int hi = inb(dma_count_port[c]);
        int v = ((hi << 8) | lo) + 1;
        if (prev >= 0 && (v - prev < 8 && prev - v < 8)) return v;
        prev = v;
    }
    return prev;
}

static int play_pos(void) {
    int rem = dma_remaining();
    if (rem <= 0 || rem > RING) return 0;
    return (RING - rem) % RING;
}

static void dma_program(void) {
    int c = hdma & 3;
    uint32_t phys = (uint32_t)(uintptr_t)sb_ring;
    outb(0xD4, (uint8_t)(0x04 | c));                       /* mask */
    outb(0xD8, 0);                                         /* clear flip-flop */
    outb(0xD6, (uint8_t)(0x58 | c));                       /* single, auto-init, memory -> device */
    outb(dma_addr_port[c], (uint8_t)((phys >> 1) & 0xFF));
    outb(dma_addr_port[c], (uint8_t)((phys >> 9) & 0xFF));
    outb(dma_page_port[c], (uint8_t)((phys >> 16) & 0xFE));
    outb(0xD8, 0);
    outb(dma_count_port[c], (uint8_t)((RING - 1) & 0xFF));
    outb(dma_count_port[c], (uint8_t)(((RING - 1) >> 8) & 0xFF));
    outb(0xD4, (uint8_t)c);                                /* unmask */
}

static void render_to(int from, int n) {
    while (n > 0) {
        int k = MIN(n, RING - from);
        audio_render(&sb_ring[from], k);
        from = (from + k) % RING;
        n -= k;
    }
}

static void clear_range(int from, int to) {
    while (from != to) {
        int end = to > from ? to : RING;
        memset(&sb_ring[from], 0, sizeof(int16_t) * (size_t)(end - from));
        from = end % RING;
    }
}

static void sb_start(void) {
    dsp_reset(base);
    memset(sb_ring, 0, sizeof sb_ring);
    render_to(0, lead);
    wpos = lead % RING;
    last_ppos = 0;
    dma_program();
    dsp_write(0xD1);                                       /* speaker on */
    dsp_write(0x41);                                       /* output sample rate */
    dsp_write((uint8_t)(RATE >> 8));
    dsp_write((uint8_t)(RATE & 0xFF));
    dsp_write(0xB6);                                       /* 16-bit DAC, auto-init, FIFO */
    dsp_write(0x10);                                       /* mono, signed */
    dsp_write((uint8_t)((BLOCK - 1) & 0xFF));
    dsp_write((uint8_t)(((BLOCK - 1) >> 8) & 0xFF));
}

static void sb_stop(void) {
    dsp_write(0xD5);                                       /* pause 16-bit DMA */
    dsp_write(0xD9);                                       /* leave auto-init */
    outb(0xD4, (uint8_t)(0x04 | (hdma & 3)));              /* mask the DMA channel */
    dsp_reset(base);
}

static void sb_pump(void) {
    int ppos = play_pos();
    if (ppos != last_ppos) audio_note_progress();
    clear_range(last_ppos, ppos);                         /* played: now silence */
    last_ppos = ppos;
    int ahead = (wpos - ppos + RING) % RING;
    if (ahead > RING - CHUNK_S * 2 || ahead == 0) {
        /* playback caught up with us: start again just ahead of it */
        audio_note_underrun();
        if (lead < LEAD_MAX) lead += 512;
        wpos = (ppos + CHUNK_S) % RING;
        ahead = CHUNK_S;
    }
    for (int i = 0; i < 4 && ahead < lead; i++) {
        render_to(wpos, CHUNK_S);
        wpos = (wpos + CHUNK_S) % RING;
        ahead += CHUNK_S;
    }
}

const sound_card_t *sb16_probe(void) {
    static const uint16_t ports[] = { 0x220, 0x240 };
    base = 0;
    for (size_t i = 0; i < ARRAY_SIZE(ports) && !base; i++)
        if (dsp_reset(ports[i])) base = ports[i];
    if (!base) return NULL;
    dsp_write(0xE1);
    int major = dsp_read(), minor = dsp_read();
    if (major < 4) {
        klog("sb16: DSP at %x is version %d.%02d, not a Sound Blaster 16 - not used", base, major, minor);
        return NULL;
    }
    uint32_t phys = (uint32_t)(uintptr_t)sb_ring;
    if (phys + sizeof sb_ring > 0x1000000u || (phys >> 17) != ((phys + sizeof sb_ring - 1) >> 17)) {
        klog("sb16: DMA buffer at %p is not ISA DMA capable - not used", (void *)sb_ring);
        return NULL;
    }
    uint8_t irqsel = mixer_read(0x80), dmasel = mixer_read(0x81);
    if (irqsel & 0x02) irq = 5;
    else if (irqsel & 0x04) irq = 7;
    else if (irqsel & 0x08) irq = 10;
    else if (irqsel & 0x01) irq = 9;      /* "IRQ 2" is IRQ 9 on the AT cascade */
    if (dmasel & 0x20) hdma = 5;
    else if (dmasel & 0x40) hdma = 6;
    else if (dmasel & 0x80) hdma = 7;
    else {
        klog("sb16: no 16-bit DMA channel configured (mixer 0x81 = %02x) - not used", dmasel);
        return NULL;
    }
    mixer_write(0x30, 0xF8);              /* master left/right 0 dB */
    mixer_write(0x31, 0xF8);
    mixer_write(0x32, 0xF8);              /* voice (DAC) left/right 0 dB */
    mixer_write(0x33, 0xF8);
    if (irq >= 0) irq_install(irq, sb_irq);
    klog("sb16: DSP %d.%02d at %x, irq %d, 16-bit dma %d, %d Hz mono", major, minor, base, irq, hdma, RATE);

    sb_card.name = "Sound Blaster 16";
    sb_card.short_name = "SB16";
    sb_card.rate = RATE;
    sb_card.start = sb_start;
    sb_card.stop = sb_stop;
    sb_card.pump = sb_pump;
    return &sb_card;
}
