/*
 * persist.c - keep the VFS on the installed hard disk.
 *
 * Active only when the kernel was started by the NovaOS disk loader with
 * "boot=disk diskid=XXXXXXXX" on its command line (the installer writes that
 * into the install header). At boot (INIT_FS) the newest valid save is read
 * and handed to vfs_deserialize(). Afterwards the tree is written back:
 *   - a few seconds after the last change (vfs sync hook),
 *   - every 60 s while there are unsaved changes,
 *   - before reboot / shut down (shutdown hook, synchronous),
 *   - on request: persist_save() (synchronous) / persist_save_async().
 * Background saves serialise the tree into a buffer, then write it in slices of
 * a few milliseconds from a timer so the GUI stays responsive.
 *
 * Two slots (A/B): a save goes to the slot not holding the newest save and is
 * committed by writing that slot's header sector last, so an interrupted save
 * (power cut) leaves the previous one intact.
 */
#include "kernel.h"
#include <nova/disk.h>
#include "disk/diskfmt.h"

#define SYNC_DELAY_MS     4000      /* save this long after the last change   */
#define PERIODIC_MS       60000     /* and at least this often while dirty     */
#define SLICE_MS          8         /* disk time per background slice          */
#define SLICE_SECTORS     32        /* sectors per disk_write call             */

/* ---- CRC32 (IEEE 802.3, reflected) ------------------------------------------------ */
static uint32_t crc_table[256];

uint32_t crc32_update(uint32_t crc, const void *data, uint32_t len) {
    if (!crc_table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- install header ----------------------------------------------------------------- */
bool persist_read_header(int disk, disk_header_t *hdr) {
    if (disk_read(disk, DISK_HDR_LBA, 1, hdr) != DISK_OK) return false;
    if (hdr->magic0 != DISK_HDR_MAGIC0 || hdr->magic1 != DISK_HDR_MAGIC1) return false;
    if (hdr->version != DISK_HDR_VERSION || hdr->header_size != sizeof *hdr) return false;
    if (crc32_buf(hdr, 508) != hdr->checksum) return false;
    const disk_info_t *d = disk_get(disk);
    if (!d || hdr->persist_slots != 2 || hdr->persist_slot_sectors < 16 ||
        (uint64_t)hdr->persist_lba + 2ull * hdr->persist_slot_sectors > d->sectors)
        return false;
    return true;
}

int persist_find_disk(uint32_t disk_id, disk_header_t *hdr_out) {
    disk_header_t *h = kmalloc(sizeof *h);
    if (!h) return -1;
    int found = -1;
    for (int i = 0; i < disk_count() && found < 0; i++) {
        if (!persist_read_header(i, h)) continue;
        if (disk_id && h->disk_id != disk_id) continue;
        found = i;
        if (hdr_out) *hdr_out = *h;
    }
    kfree(h);
    return found;
}

/* ---- state -------------------------------------------------------------------------- */
static struct {
    bool active, loaded;
    int disk;
    disk_header_t hdr;
    uint32_t saved_gen;       /* vfs_generation() of the data on disk          */
    uint32_t seq;             /* sequence number of the newest save            */
    int newest;               /* slot holding the newest save, -1 = none       */
    uint32_t last_save, saves, used;
    int last_error;
    bool failed_notified;
    /* background save */
    bool saving, again;
    int timer;
    uint8_t *buf;
    uint32_t len, nsec, done, gen, crc, started;
    int slot;
} P = { .disk = -1, .newest = -1 };

int persist_boot_disk(void) { return P.active ? P.disk : -1; }
bool persist_active(void) { return P.active; }

static uint32_t capacity(void) { return P.active ? (P.hdr.persist_slot_sectors - 1) * 512 : 0; }
static uint32_t slot_lba(int slot) { return P.hdr.persist_lba + (uint32_t)slot * P.hdr.persist_slot_sectors; }
static bool dirty(void) { return P.active && vfs_generation() != P.saved_gen; }

void persist_get_info(persist_info_t *o) {
    memset(o, 0, sizeof *o);
    o->active = P.active;
    o->disk = P.active ? P.disk : -1;
    o->dirty = dirty();
    o->saving = P.saving;
    o->loaded = P.loaded;
    o->last_save = P.last_save;
    o->saves = P.saves;
    o->capacity = capacity();
    o->used = P.used;
    o->last_error = P.last_error;
}

static bool read_slot_header(int slot, persist_header_t *ph) {
    if (disk_read(P.disk, slot_lba(slot), 1, ph) != DISK_OK) return false;
    if (ph->magic0 != PERSIST_MAGIC0 || ph->magic1 != PERSIST_MAGIC1 || ph->version != PERSIST_VERSION) return false;
    if (crc32_buf(ph, 508) != ph->checksum) return false;
    if (ph->disk_id != P.hdr.disk_id || ph->length == 0 || ph->length > capacity()) return false;
    return true;
}

/* ---- loading at boot ------------------------------------------------------------------- */
static bool load_slot(int slot, const persist_header_t *ph) {
    uint32_t nsec = (ph->length + 511) / 512;
    uint8_t *buf = kmalloc(nsec * 512);
    if (!buf) { klog("persist: no memory for %u byte image", ph->length); return false; }
    uint32_t t0 = g_ticks;
    int r = disk_read(P.disk, slot_lba(slot) + 1, nsec, buf);
    bool ok = false;
    if (r != DISK_OK) {
        klog("persist: reading slot %c failed: %s", 'A' + slot, disk_strerror(r));
    } else if (crc32_buf(buf, ph->length) != ph->data_crc) {
        klog("persist: slot %c checksum mismatch", 'A' + slot);
    } else if ((r = vfs_deserialize(buf, ph->length)) != 0) {
        klog("persist: slot %c image rejected (%d)", 'A' + slot, r);
    } else {
        ok = true;
        klog("persist: restored %u bytes from slot %c (save #%u) in %u ms", ph->length, 'A' + slot, ph->seq,
             g_ticks - t0);
    }
    kfree(buf);
    return ok;
}

static void refresh_system_files(void) {
    /* the saved tree may come from an older build: keep the system info current */
    static const char *dirs[] = { "/home/user/Desktop", "/home/user/Documents", "/home/user/Pictures",
                                  "/home/user/Music", "/home/user/Downloads", "/system", "/tmp" };
    for (int i = 0; i < ARRAY_SIZE(dirs); i++)
        if (!vfs_is_dir(dirs[i])) vfs_mkdirs(dirs[i]);
    char buf[256];
    snprintf(buf, sizeof buf, "%s %s\nKernel %s\nBuilt %s %s\n", sys_version(), sys_edition(), sys_kernel_version(), __DATE__, __TIME__);
    void *old = vfs_read_file("/system/version.txt", NULL);
    if (!old || strcmp(old, buf)) vfs_write_file("/system/version.txt", buf, (uint32_t)strlen(buf));
    kfree(old);
}

static void sync_hook(void);
static void periodic(void *ctx);
static void shutdown_hook(void);

static void persist_init(void) {
    const char *boot = sys_cmdline_get("boot");
    if (!boot || strcmp(boot, "disk")) {
        klog("persist: off (live system - files stay in memory)");
        return;
    }
    const char *ids = sys_cmdline_get("diskid");
    uint32_t id = ids ? (uint32_t)strtoul(ids, NULL, 16) : 0;
    int d = persist_find_disk(id, &P.hdr);
    if (d < 0 && id) d = persist_find_disk(0, &P.hdr);
    if (d < 0) {
        klog("persist: installed disk (id %08x) not found - files will not be saved", id);
        return;
    }
    P.active = true;
    P.disk = d;
    klog("persist: using disk %d (id %08x), 2 slots of %u KiB at lba %u", d, P.hdr.disk_id,
         P.hdr.persist_slot_sectors / 2, P.hdr.persist_lba);

    persist_header_t *ph[2];
    ph[0] = kmalloc(sizeof(persist_header_t));
    ph[1] = kmalloc(sizeof(persist_header_t));
    bool valid[2] = { false, false };
    for (int s = 0; s < 2; s++)
        if (ph[s]) valid[s] = read_slot_header(s, ph[s]);
    /* try the newest valid slot first, then the other one */
    int order[2] = { 0, 1 };
    if (valid[0] && valid[1] && ph[1]->seq > ph[0]->seq) { order[0] = 1; order[1] = 0; }
    else if (!valid[0] && valid[1]) { order[0] = 1; order[1] = 0; }
    for (int i = 0; i < 2; i++) {
        int s = order[i];
        if (!valid[s]) continue;
        if (load_slot(s, ph[s])) {
            P.loaded = true;
            P.newest = s;
            P.seq = ph[s]->seq;
            P.used = ph[s]->length;
            P.last_save = ph[s]->save_time;
            break;
        }
    }
    /* never reuse a sequence number, even of a damaged slot */
    for (int s = 0; s < 2; s++)
        if (valid[s] && ph[s]->seq > P.seq) P.seq = ph[s]->seq;
    kfree(ph[0]);
    kfree(ph[1]);
    if (!P.loaded) klog("persist: no saved files on disk yet - starting with the default files");
    refresh_system_files();
    P.saved_gen = P.loaded ? vfs_generation() : vfs_generation() - 1;   /* first boot: save defaults soon */

    vfs_set_sync_hook(sync_hook, SYNC_DELAY_MS);
    timer_add(PERIODIC_MS, periodic, NULL, true);
    sys_add_shutdown_hook(shutdown_hook);
    if (!P.loaded) persist_save_async();
}
REGISTER_INIT(INIT_FS, persist_init);

/* ---- saving ------------------------------------------------------------------------------ */
static int serialize(uint8_t **out, uint32_t *len, uint32_t *gen) {
    *gen = vfs_generation();
    int32_t need = vfs_serialize(NULL, 0);
    if (need <= 0) return DISK_EIO;
    if ((uint32_t)need > capacity()) return DISK_ENOSPC;
    uint32_t alloc = ((uint32_t)need + 511) & ~511u;
    uint8_t *buf = kmalloc(alloc);
    if (!buf) return DISK_ENOMEM;
    int32_t got = vfs_serialize(buf, (uint32_t)need);
    if (got != need) { kfree(buf); return DISK_EIO; }
    memset(buf + got, 0, alloc - (uint32_t)got);
    *out = buf;
    *len = (uint32_t)got;
    return DISK_OK;
}

static int commit(int slot, uint32_t len, uint32_t crc) {
    int r = disk_flush(P.disk);
    if (r) return r;
    persist_header_t *ph = kcalloc(1, sizeof *ph);
    if (!ph) return DISK_ENOMEM;
    ph->magic0 = PERSIST_MAGIC0;
    ph->magic1 = PERSIST_MAGIC1;
    ph->version = PERSIST_VERSION;
    ph->disk_id = P.hdr.disk_id;
    ph->seq = P.seq + 1;
    ph->length = len;
    ph->data_crc = crc;
    ph->save_time = sys_time();
    ph->checksum = crc32_buf(ph, 508);
    r = disk_write(P.disk, slot_lba(slot), 1, ph);
    if (!r) r = disk_flush(P.disk);
    if (!r) {
        P.seq = ph->seq;
        P.newest = slot;
        P.last_save = ph->save_time;
        P.used = len;
        P.saves++;
    }
    kfree(ph);
    return r;
}

static void report(int r, uint32_t len, uint32_t t0, const char *how) {
    P.last_error = r;
    if (r == DISK_OK) {
        klog("persist: saved %u bytes to slot %c (save #%u, %s) in %u ms", len, 'A' + P.newest, P.seq, how,
             g_ticks - t0);
        P.failed_notified = false;
    } else {
        klog("persist: save failed: %s", disk_strerror(r));
        if (!P.failed_notified) {
            P.failed_notified = true;
            ui_notify("Files not saved", r == DISK_ENOSPC ? "The file storage on the disk is full."
                                                          : "Could not write your files to the disk.");
        }
    }
}

static void cancel_async(void) {
    if (P.timer) timer_cancel(P.timer);
    P.timer = 0;
    kfree(P.buf);
    P.buf = NULL;
    P.saving = false;
}

int persist_save(void) {
    if (!P.active) return DISK_ENODEV;
    bool was_saving = P.saving;
    cancel_async();
    if (!was_saving && !dirty()) return DISK_OK;
    uint32_t t0 = g_ticks, len = 0, gen;
    uint8_t *buf;
    int r = serialize(&buf, &len, &gen);
    if (r == DISK_OK) {
        int slot = P.newest == 0 ? 1 : 0;
        r = disk_write(P.disk, slot_lba(slot) + 1, (len + 511) / 512, buf);
        if (!r) r = commit(slot, len, crc32_buf(buf, len));
        kfree(buf);
        if (!r) P.saved_gen = gen;
    }
    report(r, len, t0, "sync");
    return r;
}

static void async_tick(void *ctx) {
    (void)ctx;
    if (!P.saving) return;
    uint32_t t0 = g_ticks;
    int r = DISK_OK;
    while (P.done < P.nsec && g_ticks - t0 < SLICE_MS) {
        uint32_t n = MIN(SLICE_SECTORS, P.nsec - P.done);
        r = disk_write(P.disk, slot_lba(P.slot) + 1 + P.done, n, P.buf + P.done * 512);
        if (r) break;
        /* checksum computed slice by slice too, so no single step takes long */
        uint32_t off = P.done * 512, cl = MIN(n * 512, P.len - MIN(off, P.len));
        P.crc = crc32_update(P.crc, P.buf + off, cl);
        P.done += n;
    }
    if (r == DISK_OK && P.done < P.nsec) return;     /* more next tick */
    uint32_t len = P.len, gen = P.gen, crc = P.crc, started = P.started;
    if (r == DISK_OK) r = commit(P.slot, len, crc);
    cancel_async();
    if (r == DISK_OK) P.saved_gen = gen;
    report(r, len, started, "background");
    if (r == DISK_OK && P.again && dirty()) { P.again = false; persist_save_async(); }
}

void persist_save_async(void) {
    if (!P.active) return;
    if (P.saving) { P.again = true; return; }
    if (!dirty()) return;
    P.again = false;
    int r = serialize(&P.buf, &P.len, &P.gen);
    if (r) { report(r, 0, g_ticks, "background"); return; }
    P.crc = 0;
    P.nsec = (P.len + 511) / 512;
    P.done = 0;
    P.slot = P.newest == 0 ? 1 : 0;
    P.started = g_ticks;
    P.saving = true;
    P.timer = timer_add(5, async_tick, NULL, true);
    if (!P.timer) { cancel_async(); persist_save(); }
}

static void sync_hook(void) { persist_save_async(); }

static void periodic(void *ctx) {
    (void)ctx;
    if (dirty() && !P.saving) persist_save_async();
}

static void shutdown_hook(void) {
    if (!P.active || (!dirty() && !P.saving)) return;
    if (g_fb.addr) power_screen("Saving your files...", "Please wait");
    persist_save();
}
