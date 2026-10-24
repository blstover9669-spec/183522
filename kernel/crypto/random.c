/*
 * random.c - secure random numbers for TLS keys, TCP sequence numbers, DNS ids.
 *
 * Seeding: the CPU's hardware generator (RDSEED / RDRAND) when CPUID reports it,
 * plus timing jitter of the time stamp counter (TSC) against I/O port reads and
 * the PIT, the RTC and uptime. Later, the arrival times of network packets and
 * other events are mixed in (random_add_timing).
 *
 * Generator: a SHA-256 based construction. key = SHA-256 of everything seen so
 * far; output block i = SHA-256(key || counter || "out"); after every request the
 * key is replaced by SHA-256(key || counter || "next"), so earlier output cannot
 * be recomputed from a later state (forward secrecy).
 */
#include "kernel.h"
#include "crypto.h"

static uint8_t key[32];
static uint64_t counter;
static bool seeded;
static const char *source = "TSC jitter";
static uint64_t pending[16];
static int npending;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static bool hw_rdrand(uint32_t *v) {
    uint8_t ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
    return ok;
}

static bool hw_rdseed(uint32_t *v) {
    uint8_t ok;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
    return ok;
}

static void mix(const void *data, size_t n) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, key, 32);
    sha256_update(&c, data, n);
    sha256_final(&c, key);
}

static void seed(void) {
    sha256_ctx_t c;
    sha256_init(&c);
    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t maxleaf = r[0];
    cpuid(1, 0, r);
    bool has_rdrand = (r[2] >> 30) & 1, has_rdseed = false;
    if (maxleaf >= 7) { cpuid(7, 0, r); has_rdseed = (r[1] >> 18) & 1; }
    int hw = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t v;
        bool ok = false;
        for (int t = 0; t < 10 && !ok; t++) ok = has_rdseed ? hw_rdseed(&v) : has_rdrand ? hw_rdrand(&v) : false;
        if (!ok) break;
        sha256_update(&c, &v, sizeof v);
        hw++;
    }
    if (hw >= 16) source = has_rdseed ? "RDSEED + TSC jitter" : "RDRAND + TSC jitter";
    /* TSC jitter: time I/O port reads and PIT counter latches (each read takes a
       variable number of cycles in a VM and on real hardware) */
    uint64_t prev = rdtsc();
    for (int i = 0; i < 2048; i++) {
        outb(0x43, 0x00);
        uint8_t lo = inb(0x40), hi = inb(0x40);
        uint64_t now = rdtsc();
        uint32_t d[2] = { (uint32_t)(now - prev), (uint32_t)(lo | (hi << 8)) };
        sha256_update(&c, d, sizeof d);
        prev = now;
    }
    uint32_t misc[4] = { sys_time(), g_ticks, (uint32_t)rdtsc(), (uint32_t)(rdtsc() >> 32) };
    sha256_update(&c, misc, sizeof misc);
    sha256_update(&c, key, 32);
    sha256_final(&c, key);
    seeded = true;
    klog("random: seeded from %s", source);
}

const char *random_source(void) {
    if (!seeded) seed();
    return source;
}

void random_add_entropy(const void *data, size_t n) {
    if (!seeded) seed();
    mix(data, n);
}

void random_add_timing(void) {
    pending[npending++] = rdtsc();
    if (npending == ARRAY_SIZE(pending)) {
        if (!seeded) seed();
        mix(pending, sizeof pending);
        npending = 0;
    }
}

void random_bytes(void *out, size_t n) {
    if (!seeded) seed();
    uint8_t *o = out;
    uint64_t t = rdtsc();
    mix(&t, sizeof t);
    while (n) {
        sha256_ctx_t c;
        uint8_t blk[32];
        counter++;
        sha256_init(&c);
        sha256_update(&c, key, 32);
        sha256_update(&c, &counter, sizeof counter);
        sha256_update(&c, "out", 3);
        sha256_final(&c, blk);
        size_t k = n < 32 ? n : 32;
        memcpy(o, blk, k);
        o += k;
        n -= k;
        secure_zero(blk, sizeof blk);
    }
    counter++;
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, key, 32);
    sha256_update(&c, &counter, sizeof counter);
    sha256_update(&c, "next", 4);
    sha256_final(&c, key);
}

uint32_t random_u32(void) {
    uint32_t v;
    random_bytes(&v, sizeof v);
    return v;
}
