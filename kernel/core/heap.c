/*
 * heap.c - kernel heap allocator.
 *
 * Boundary-tag allocator with segregated free lists:
 *   - every block starts with a 16-byte header holding its size, the size of
 *     the physically previous block and a magic word; payloads are 16-byte
 *     aligned;
 *   - free blocks are kept in 32 power-of-two size-class lists (bin k holds
 *     sizes in [2^k, 2^(k+1))), so allocation is roughly O(1) and big
 *     multi-megabyte requests are as cheap as small ones;
 *   - neighbouring free blocks are coalesced on free.
 * Not interrupt safe: never allocate from IRQ handlers.
 */
#include "kernel.h"

#define HDR_SIZE   16u
#define MIN_BLOCK  32u
#define USED       1u
#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu
#define NBINS      32

typedef struct block {
    uint32_t size;        /* total size incl. header; bit 0 = used */
    uint32_t prev_size;   /* size of previous physical block, 0 if first */
    uint32_t magic;
    uint32_t req;         /* requested size (used blocks), for stats/debug */
    /* free blocks only: */
    struct block *next_free, *prev_free;
} block_t;

static uint8_t *heap_start, *heap_end;
static block_t *bins[NBINS];
static uint32_t heap_used_bytes;
static uint32_t heap_total_bytes;
static uint32_t alloc_count;

static inline uint32_t bsize(const block_t *b) { return b->size & ~15u; }
static inline bool bused(const block_t *b) { return b->size & USED; }
static inline block_t *bnext(block_t *b) { return (block_t *)((uint8_t *)b + bsize(b)); }
static inline block_t *bprev(block_t *b) { return b->prev_size ? (block_t *)((uint8_t *)b - b->prev_size) : NULL; }

static int bin_of(uint32_t size) {
    int k = 31 - __builtin_clz(size);
    return k < NBINS ? k : NBINS - 1;
}

static void bin_insert(block_t *b) {
    int k = bin_of(bsize(b));
    b->magic = MAGIC_FREE;
    b->prev_free = NULL;
    b->next_free = bins[k];
    if (bins[k]) bins[k]->prev_free = b;
    bins[k] = b;
}

static void bin_remove(block_t *b) {
    int k = bin_of(bsize(b));
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else bins[k] = b->next_free;
    if (b->next_free) b->next_free->prev_free = b->prev_free;
}

static void set_size(block_t *b, uint32_t size, bool used) {
    b->size = size | (used ? USED : 0);
    block_t *n = (block_t *)((uint8_t *)b + size);
    if ((uint8_t *)n < heap_end) n->prev_size = size;
}

void heap_init(uintptr_t start, size_t size) {
    start = (start + 15) & ~(uintptr_t)15;
    size &= ~(size_t)15;
    heap_start = (uint8_t *)start;
    heap_end = heap_start + size;
    memset(bins, 0, sizeof bins);
    block_t *b = (block_t *)heap_start;
    b->prev_size = 0;
    set_size(b, (uint32_t)size, false);
    bin_insert(b);
    heap_total_bytes = (uint32_t)size;
    heap_used_bytes = 0;
    klog("heap: %u MiB at %p", (unsigned)(size >> 20), (void *)start);
}

static void heap_corrupt(block_t *b, const char *what) {
    kpanic("Heap corruption detected (%s) at block %p (size=%08x magic=%08x)",
           what, (void *)b, b->size, b->magic);
}

void *kmalloc(size_t req) {
    if (!heap_start) return NULL;
    if (req > 0x7FFFFFF0u) return NULL;
    uint32_t need = ((uint32_t)req + HDR_SIZE + 15u) & ~15u;
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    block_t *found = NULL;
    int k = bin_of(need);
    /* first fit within the exact bin */
    for (block_t *b = bins[k]; b; b = b->next_free) {
        if (bsize(b) >= need) { found = b; break; }
    }
    /* any block in a larger bin is big enough */
    for (int j = k + 1; !found && j < NBINS; j++)
        if (bins[j]) found = bins[j];
    if (!found) {
        klog("kmalloc: out of memory (request %u bytes, used %u of %u)",
             (unsigned)req, heap_used_bytes, heap_total_bytes);
        return NULL;
    }
    if (found->magic != MAGIC_FREE || bused(found)) heap_corrupt(found, "free list");
    bin_remove(found);
    uint32_t total = bsize(found);
    if (total - need >= MIN_BLOCK) {
        /* split: tail becomes a new free block */
        set_size(found, need, true);
        block_t *rest = bnext(found);
        rest->prev_size = need;
        set_size(rest, total - need, false);
        bin_insert(rest);
    } else {
        set_size(found, total, true);
        need = total;
    }
    found->magic = MAGIC_USED;
    found->req = (uint32_t)req;
    heap_used_bytes += need;
    alloc_count++;
    return (uint8_t *)found + HDR_SIZE;
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)((uint8_t *)p - HDR_SIZE);
    if ((uint8_t *)b < heap_start || (uint8_t *)b >= heap_end || ((uintptr_t)p & 15))
        kpanic("kfree: invalid pointer %p", p);
    if (b->magic != MAGIC_USED || !bused(b)) {
        if (b->magic == MAGIC_FREE) kpanic("kfree: double free of %p", p);
        heap_corrupt(b, "kfree header");
    }
    uint32_t size = bsize(b);
    heap_used_bytes -= size;
    alloc_count--;
    /* merge with next */
    block_t *n = bnext(b);
    if ((uint8_t *)n < heap_end && !bused(n)) {
        if (n->magic != MAGIC_FREE) heap_corrupt(n, "next block");
        bin_remove(n);
        size += bsize(n);
    }
    /* merge with previous */
    block_t *pv = bprev(b);
    if (pv && !bused(pv)) {
        if (pv->magic != MAGIC_FREE) heap_corrupt(pv, "prev block");
        bin_remove(pv);
        size += bsize(pv);
        b = pv;
    }
    set_size(b, size, false);
    bin_insert(b);
}

void *kcalloc(size_t n, size_t size) {
    if (size && n > 0xFFFFFFFFu / size) return NULL;
    size_t total = n * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

size_t kmalloc_usable_size(void *p) {
    if (!p) return 0;
    block_t *b = (block_t *)((uint8_t *)p - HDR_SIZE);
    return bsize(b) - HDR_SIZE;
}

void *krealloc(void *p, size_t req) {
    if (!p) return kmalloc(req);
    if (req == 0) { kfree(p); return NULL; }
    block_t *b = (block_t *)((uint8_t *)p - HDR_SIZE);
    if (b->magic != MAGIC_USED) heap_corrupt(b, "krealloc");
    uint32_t need = ((uint32_t)req + HDR_SIZE + 15u) & ~15u;
    if (need < MIN_BLOCK) need = MIN_BLOCK;
    uint32_t cur = bsize(b);
    if (need <= cur) {
        b->req = (uint32_t)req;
        return p;   /* shrinking: keep the block (cheap) */
    }
    /* try to grow into a free successor */
    block_t *n = bnext(b);
    if ((uint8_t *)n < heap_end && !bused(n) && cur + bsize(n) >= need) {
        uint32_t total = cur + bsize(n);
        bin_remove(n);
        heap_used_bytes -= cur;
        if (total - need >= MIN_BLOCK) {
            set_size(b, need, true);
            block_t *rest = bnext(b);
            rest->prev_size = need;
            set_size(rest, total - need, false);
            bin_insert(rest);
            heap_used_bytes += need;
        } else {
            set_size(b, total, true);
            heap_used_bytes += total;
        }
        b->req = (uint32_t)req;
        return p;
    }
    void *np = kmalloc(req);
    if (!np) return NULL;
    memcpy(np, p, cur - HDR_SIZE);
    kfree(p);
    return np;
}

bool heap_check(void) {
    block_t *b = (block_t *)heap_start;
    uint32_t prev = 0;
    while ((uint8_t *)b < heap_end) {
        if (b->prev_size != prev) { klog("heap_check: bad prev_size at %p", (void *)b); return false; }
        if (b->magic != (bused(b) ? MAGIC_USED : MAGIC_FREE)) { klog("heap_check: bad magic at %p", (void *)b); return false; }
        if (bsize(b) < MIN_BLOCK) { klog("heap_check: bad size at %p", (void *)b); return false; }
        prev = bsize(b);
        b = bnext(b);
    }
    return (uint8_t *)b == heap_end;
}

uint32_t sys_mem_free(void) { return heap_total_bytes - heap_used_bytes; }
uint32_t sys_mem_used(void) { return heap_used_bytes; }
uint32_t sys_heap_size(void) { return heap_total_bytes; }

/* ---- physical memory: pick the heap region from the multiboot memory map ---- */
extern uint8_t __kernel_end[];
uint32_t g_mem_total;

void memory_init(void) {
    uintptr_t kend = ((uintptr_t)__kernel_end + 0xFFF) & ~(uintptr_t)0xFFF;
    uint64_t best_start = 0, best_len = 0, total = 0;
    for (int i = 0; i < g_boot.mmap_count; i++) {
        if (g_boot.mmap[i].type != 1) continue;
        uint64_t s = g_boot.mmap[i].addr, e = s + g_boot.mmap[i].len;
        total += g_boot.mmap[i].len;
        if (e > 0xF0000000ull) e = 0xF0000000ull;   /* stay below typical MMIO */
        if (e <= kend) continue;
        if (s < kend) s = kend;
        if (e > s && e - s > best_len) { best_start = s; best_len = e - s; }
    }
    g_mem_total = total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)total;
    if (!best_len) {
        /* no map: fall back to mem_upper (KiB above 1 MiB) */
        uint64_t e = 0x100000ull + (uint64_t)g_boot.mem_upper_kb * 1024;
        g_mem_total = (uint32_t)(e);
        if (e > kend) { best_start = kend; best_len = e - kend; }
    }
    if (best_len < 8 * 1024 * 1024)
        kpanic("Not enough memory: need at least 16 MiB of RAM (found %u KiB usable)",
               (unsigned)(best_len / 1024));
    klog("memory: %u MiB total, kernel ends at %p", g_mem_total >> 20, (void *)kend);
    heap_init((uintptr_t)best_start, (size_t)best_len);
}

uint32_t sys_mem_total(void) { return g_mem_total; }
