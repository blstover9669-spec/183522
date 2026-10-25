/*
 * install.c - write a bootable NovaOS installation to a hard disk.
 *
 * The running kernel installs itself: the boot loader binaries are embedded
 * at build time (blobs.asm), and the kernel image is its own loaded memory
 * [__kernel_start, __data_end) with .data taken from the pristine copy that
 * boot.asm saves before any C code runs. See diskfmt.h for the layout.
 *
 * The installation uses only the drive size the user chose (install_begin_sized):
 * the partition ends there and the two file storage slots share the space after
 * the kernel (each slot just under 2 GiB at most).
 *
 * The work is split into slices (install_step) so the Installer app can drive
 * it from a timer and keep the GUI responsive. Order of writes: stage 2,
 * kernel, files, storage headers, verify (read back + compare), and only then
 * the install header and the MBR - an interrupted installation never leaves
 * a disk that tries to boot a half-written system.
 */
#include "kernel.h"
#include <nova/disk.h>
#include "disk/diskfmt.h"

extern const uint8_t bootblob_mbr[], bootblob_mbr_end[];
extern const uint8_t bootblob_stage2[], bootblob_stage2_end[];
extern uint8_t __kernel_start[], __data_start[], __data_end[], __data_snapshot[];
extern void _start(void);

#define SLOT_MIN_SECTORS  (4u * 1024 * 1024 / 512)      /* 4 MiB per slot at least  */
/* At most just under 2 GiB per slot: a saved image is an int32-sized VFS image, and the
   files live in RAM while NovaOS runs, so a bigger slot could never be filled. */
#define SLOT_MAX_SECTORS  (2047u * 1024 * 1024 / 512)
#define IO_SECTORS        32

/* ---- drive sizes offered by the NovaOS Drive installation and the Installer app ------ */
static const uint32_t size_mib[] = { 100, 150, 200, 250, 500, 700, 1024, 5 * 1024, 10 * 1024 };
static const char *const size_names[] = { "100 MB", "150 MB", "200 MB", "250 MB", "500 MB",
                                          "700 MB", "1 GB", "5 GB", "10 GB" };

int install_size_count(void) { return ARRAY_SIZE(size_mib); }
uint64_t install_size_bytes(int i) {
    return (i >= 0 && i < ARRAY_SIZE(size_mib)) ? (uint64_t)size_mib[i] << 20 : 0;
}
const char *install_size_label(int i) { return (i >= 0 && i < ARRAY_SIZE(size_names)) ? size_names[i] : ""; }

bool install_size_fits(int disk, uint64_t bytes) {
    const disk_info_t *d = disk_get(disk);
    return d && bytes >= INSTALL_MIN_BYTES && bytes <= d->sectors * 512;
}

int install_size_default(int disk) {
    int best = -1;
    for (int i = 0; i < ARRAY_SIZE(size_mib); i++)
        if (install_size_fits(disk, install_size_bytes(i))) best = i;
    return best;
}

static uint32_t kernel_bytes(void) { return (uint32_t)(__data_end - __kernel_start); }
static uint32_t stage2_bytes(void) { return (uint32_t)(bootblob_stage2_end - bootblob_stage2); }

uint32_t install_system_bytes(void) { return 1024 + ((stage2_bytes() + 511) & ~511u) + ((kernel_bytes() + 511) & ~511u); }

typedef struct layout {
    uint64_t total_sectors;       /* sectors used by the installation (chosen size) */
    uint32_t kernel_sectors, persist_lba, slot_sectors;
} layout_t;

/* size_sectors: the drive size chosen by the user, 0 = the whole disk */
static int compute_layout(const disk_info_t *d, uint64_t size_sectors, layout_t *l, char *why, int n) {
    if (size_sectors > d->sectors) {
        if (why) snprintf(why, (size_t)n, "The disk is smaller than the chosen drive size.");
        return DISK_ENOSPC;
    }
    l->total_sectors = size_sectors ? size_sectors : d->sectors;
    l->kernel_sectors = (kernel_bytes() + 511) / 512;
    /* 1 MiB of slack after the kernel, then align to 1 MiB */
    l->persist_lba = (DISK_KERNEL_LBA + l->kernel_sectors + 2048 + 2047) & ~2047u;
    uint64_t avail = l->total_sectors > l->persist_lba ? l->total_sectors - l->persist_lba : 0;
    uint64_t slot = avail / 2;
    if (slot > SLOT_MAX_SECTORS) slot = SLOT_MAX_SECTORS;
    l->slot_sectors = (uint32_t)slot;
    if (l->total_sectors * 512 < INSTALL_MIN_BYTES || slot < SLOT_MIN_SECTORS) {
        char need[24];
        disk_format_size(INSTALL_MIN_BYTES, need, sizeof need);
        if (why) snprintf(why, (size_t)n, "The disk is too small (NovaOS needs at least %s).", need);
        return DISK_ENOSPC;
    }
    if (!d->lba48 && (uint64_t)l->persist_lba + 2ull * l->slot_sectors > 0x0FFFFFFFull) {
        uint64_t max = (0x0FFFFFFFull - l->persist_lba) / 2;
        l->slot_sectors = (uint32_t)MIN(max, (uint64_t)l->slot_sectors);
    }
    return DISK_OK;
}

bool install_disk_has_novaos(int disk) {
    disk_header_t *h = kmalloc(sizeof *h);
    if (!h) return false;
    bool r = persist_read_header(disk, h);
    kfree(h);
    return r;
}

bool install_is_boot_disk(int disk) { return disk >= 0 && persist_boot_disk() == disk; }

int install_check_disk(int disk, char *why, int n) {
    const disk_info_t *d = disk_get(disk);
    if (!d) {
        if (why) snprintf(why, (size_t)n, "No such disk.");
        return DISK_ENODEV;
    }
    if (install_is_boot_disk(disk)) {
        if (why) snprintf(why, (size_t)n, "NovaOS is running from this disk.");
        return DISK_EBUSY;
    }
    layout_t l;
    return compute_layout(d, 0, &l, why, n);
}

/* ---- job ------------------------------------------------------------------------------- */
enum { PH_STAGE2, PH_KERNEL, PH_FILES, PH_STORAGE, PH_VERIFY, PH_BOOT, PH_DONE, PH_FAILED };

static const char *const phase_names[] = {
    "Writing the boot loader", "Copying NovaOS", "Copying your files", "Preparing file storage",
    "Verifying", "Making the disk bootable", "Finished", "Failed",
};

struct install_job {
    int disk;
    int phase;
    uint32_t pos;                 /* sectors done in the current phase       */
    uint32_t done, total;         /* weighted sector counts for the progress */
    layout_t l;
    uint8_t *kernel;              /* flat kernel image, sector padded        */
    uint8_t *stage2;
    uint32_t stage2_sectors;
    uint8_t *files;               /* vfs_serialize() image or NULL           */
    uint32_t files_len, files_sectors;
    uint8_t *verify;              /* IO_SECTORS read-back buffer             */
    disk_header_t hdr;
    uint8_t mbr[512];
    char error[160];
};

static void chs(uint32_t lba, uint8_t *out) {
    uint32_t c = lba / (255 * 63), h = (lba / 63) % 255, s = lba % 63 + 1;
    if (c > 1023) { c = 1023; h = 254; s = 63; }
    out[0] = (uint8_t)h;
    out[1] = (uint8_t)(s | ((c >> 2) & 0xC0));
    out[2] = (uint8_t)c;
}

/* Boot options carried over from the current command line into the installed one. */
static void build_cmdline(char *out, size_t n, uint32_t disk_id) {
    snprintf(out, n, "boot=disk diskid=%08x", disk_id);
    static const char *carry[] = { "testmouse", "res", "mousespeed", "nosplash" };
    for (int i = 0; i < ARRAY_SIZE(carry); i++) {
        const char *v = sys_cmdline_get(carry[i]);
        if (!v) continue;
        size_t len = strlen(out);
        if (*v) snprintf(out + len, n - len, " %s=%s", carry[i], v);
        else snprintf(out + len, n - len, " %s", carry[i]);
    }
}

install_job_t *install_begin(int disk, bool copy_files, char *err, int n) {
    return install_begin_sized(disk, 0, copy_files, err, n);
}

install_job_t *install_begin_sized(int disk, uint64_t size_bytes, bool copy_files, char *err, int n) {
    if (install_check_disk(disk, err, n) != DISK_OK) return NULL;
    const disk_info_t *d = disk_get(disk);
    layout_t l;
    if (compute_layout(d, size_bytes / 512, &l, err, n) != DISK_OK) return NULL;
    install_job_t *j = kcalloc(1, sizeof *j);
    if (!j) goto nomem;
    j->disk = disk;
    j->l = l;

    /* kernel image: read-only part straight from memory, .data from the boot-time copy */
    uint32_t kb = kernel_bytes(), ksz = j->l.kernel_sectors * 512;
    j->kernel = kcalloc(1, ksz);
    if (!j->kernel) goto nomem;
    uint32_t ro = (uint32_t)(__data_start - __kernel_start);
    memcpy(j->kernel, __kernel_start, ro);
    memcpy(j->kernel + ro, __data_snapshot, kb - ro);

    j->stage2_sectors = (stage2_bytes() + 511) / 512;
    if (j->stage2_sectors == 0 || j->stage2_sectors > DISK_STAGE2_MAX) {
        snprintf(err, (size_t)n, "The boot loader is too large.");
        install_free(j);
        return NULL;
    }
    j->stage2 = kcalloc(1, j->stage2_sectors * 512);
    j->verify = kmalloc(IO_SECTORS * 512);
    if (!j->stage2 || !j->verify) goto nomem;
    memcpy(j->stage2, bootblob_stage2, stage2_bytes());

    if (copy_files) {
        int32_t need = vfs_serialize(NULL, 0);
        if (need > 0 && (uint32_t)need <= (j->l.slot_sectors - 1) * 512) {
            j->files_sectors = ((uint32_t)need + 511) / 512;
            j->files = kcalloc(1, j->files_sectors * 512);
            if (!j->files) goto nomem;
            j->files_len = (uint32_t)vfs_serialize(j->files, (uint32_t)need);
        } else {
            klog("install: current files (%d bytes) do not fit - not copied", (int)need);
        }
    }

    uint32_t disk_id = (uint32_t)rand() ^ (sys_time() << 8) ^ g_ticks ^ 0x4E4F5641u;
    if (!disk_id) disk_id = 0x4E4F5641u;

    disk_header_t *h = &j->hdr;
    h->magic0 = DISK_HDR_MAGIC0;
    h->magic1 = DISK_HDR_MAGIC1;
    h->version = DISK_HDR_VERSION;
    h->header_size = sizeof *h;
    h->stage2_lba = DISK_STAGE2_LBA;
    h->stage2_sectors = j->stage2_sectors;
    h->kernel_lba = DISK_KERNEL_LBA;
    h->kernel_sectors = j->l.kernel_sectors;
    h->kernel_load = (uint32_t)__kernel_start;
    h->kernel_entry = (uint32_t)_start;
    h->kernel_bytes = kb;
    h->persist_lba = j->l.persist_lba;
    h->persist_slot_sectors = j->l.slot_sectors;
    h->persist_slots = 2;
    h->disk_id = disk_id;
    h->install_time = sys_time();
    build_cmdline(h->cmdline, sizeof h->cmdline, disk_id);
    strlcpy(h->version_str, sys_version(), sizeof h->version_str);
    h->kernel_crc = crc32_buf(j->kernel, kb);
    h->drive_sectors = (uint32_t)MIN(j->l.total_sectors, 0xFFFFFFFFull);
    h->checksum = crc32_buf(h, 508);

    /* MBR: stage 1 code, disk signature, one active partition, boot signature */
    memcpy(j->mbr, bootblob_mbr, 440);
    memcpy(j->mbr + 440, &disk_id, 4);
    uint8_t *pe = j->mbr + 446;
    uint64_t part_len = j->l.total_sectors - DISK_KERNEL_LBA;
    if (part_len > 0xFFFFFFFFull) part_len = 0xFFFFFFFFull;
    pe[0] = 0x80;
    chs(DISK_KERNEL_LBA, pe + 1);
    pe[4] = DISK_PART_TYPE;
    chs((uint32_t)MIN(j->l.total_sectors - 1, 0xFFFFFFFFull), pe + 5);
    uint32_t start = DISK_KERNEL_LBA, len32 = (uint32_t)part_len;
    memcpy(pe + 8, &start, 4);
    memcpy(pe + 12, &len32, 4);
    j->mbr[510] = 0x55;
    j->mbr[511] = 0xAA;

    j->total = j->stage2_sectors + j->l.kernel_sectors + j->files_sectors + 2 +
               (j->stage2_sectors + j->l.kernel_sectors) + 2;
    j->phase = PH_STAGE2;
    klog("install: to disk %d (%s), drive %u MiB, kernel %u bytes, files %u bytes, storage 2 x %u KiB at lba %u, "
         "cmdline \"%s\"", disk, d->model, (uint32_t)(j->l.total_sectors / 2048), kb, j->files_len,
         j->l.slot_sectors / 2, j->l.persist_lba, h->cmdline);
    return j;

nomem:
    if (err) snprintf(err, (size_t)n, "Not enough memory to prepare the installation.");
    install_free(j);
    return NULL;
}

static int fail(install_job_t *j, int r, const char *what) {
    snprintf(j->error, sizeof j->error, "%s: %s.", what, disk_strerror(r));
    klog("install: FAILED - %s", j->error);
    j->phase = PH_FAILED;
    return r;
}

/* write `total` sectors of `data` at `lba`, continuing at j->pos; true when the phase is complete */
static int write_part(install_job_t *j, uint32_t lba, const uint8_t *data, uint32_t total, bool *complete) {
    uint32_t n = MIN(IO_SECTORS, total - j->pos);
    int r = n ? disk_write(j->disk, lba + j->pos, n, data + j->pos * 512) : DISK_OK;
    if (r) return r;
    j->pos += n;
    j->done += n;
    *complete = j->pos >= total;
    return DISK_OK;
}

int install_step(install_job_t *j, uint32_t budget_ms) {
    if (!j) return DISK_EINVAL;
    if (j->phase == PH_DONE) return 1;
    if (j->phase == PH_FAILED) return DISK_EIO;
    uint32_t t0 = g_ticks;
    do {
        bool complete = false;
        int r = DISK_OK;
        switch (j->phase) {
        case PH_STAGE2:
            if (j->pos == 0) {
                /* an old installation stops being "installed" (GRUB boots the CD) until
                   this one is complete */
                memset(j->verify, 0, 512);
                r = disk_write(j->disk, DISK_SIG_LBA, 1, j->verify);
                if (r) return fail(j, r, "Writing the boot loader failed");
            }
            r = write_part(j, DISK_STAGE2_LBA, j->stage2, j->stage2_sectors, &complete);
            if (r) return fail(j, r, "Writing the boot loader failed");
            break;
        case PH_KERNEL:
            r = write_part(j, DISK_KERNEL_LBA, j->kernel, j->l.kernel_sectors, &complete);
            if (r) return fail(j, r, "Copying NovaOS failed");
            break;
        case PH_FILES:
            r = write_part(j, j->l.persist_lba + 1, j->files, j->files_sectors, &complete);
            if (r) return fail(j, r, "Copying your files failed");
            break;
        case PH_STORAGE: {
            /* slot A: the copied files (or an empty header); slot B: empty */
            persist_header_t *ph = kcalloc(2, sizeof *ph);
            if (!ph) return fail(j, DISK_ENOMEM, "Preparing file storage failed");
            if (j->files) {
                ph->magic0 = PERSIST_MAGIC0;
                ph->magic1 = PERSIST_MAGIC1;
                ph->version = PERSIST_VERSION;
                ph->disk_id = j->hdr.disk_id;
                ph->seq = 1;
                ph->length = j->files_len;
                ph->data_crc = crc32_buf(j->files, j->files_len);
                ph->save_time = sys_time();
                ph->checksum = crc32_buf(ph, 508);
            }
            r = disk_write(j->disk, j->l.persist_lba, 1, &ph[0]);
            if (!r) r = disk_write(j->disk, j->l.persist_lba + j->l.slot_sectors, 1, &ph[1]);
            if (!r) r = disk_flush(j->disk);
            kfree(ph);
            if (r) return fail(j, r, "Preparing file storage failed");
            j->done += 2;
            complete = true;
            break;
        }
        case PH_VERIFY: {
            /* read back stage 2 and the kernel and compare with what we wrote */
            uint32_t all = j->stage2_sectors + j->l.kernel_sectors;
            uint32_t n = MIN(IO_SECTORS, all - j->pos);
            uint32_t p = j->pos;
            uint32_t lba;
            const uint8_t *ref;
            if (p < j->stage2_sectors) {
                n = MIN(n, j->stage2_sectors - p);
                lba = DISK_STAGE2_LBA + p;
                ref = j->stage2 + p * 512;
            } else {
                lba = DISK_KERNEL_LBA + (p - j->stage2_sectors);
                ref = j->kernel + (p - j->stage2_sectors) * 512;
            }
            r = disk_read(j->disk, lba, n, j->verify);
            if (r) return fail(j, r, "Verifying the installation failed");
            if (memcmp(j->verify, ref, n * 512)) {
                klog("install: verify mismatch at lba %u", lba);
                return fail(j, DISK_ECORRUPT, "Verifying the installation failed");
            }
            j->pos += n;
            j->done += n;
            complete = j->pos >= all;
            break;
        }
        case PH_BOOT:
            r = disk_write(j->disk, DISK_HDR_LBA, 1, &j->hdr);
            if (!r) r = disk_flush(j->disk);
            if (!r) {
                memset(j->verify, 0, 512);
                memcpy(j->verify, DISK_SIGNATURE, sizeof DISK_SIGNATURE - 1);
                r = disk_write(j->disk, DISK_SIG_LBA, 1, j->verify);
            }
            if (!r) r = disk_write(j->disk, 0, 1, j->mbr);
            if (!r) r = disk_flush(j->disk);
            if (r) return fail(j, r, "Making the disk bootable failed");
            j->done += 2;
            complete = true;
            break;
        default:
            return 1;
        }
        if (complete) {
            j->phase++;
            j->pos = 0;
            if (j->phase == PH_FILES && !j->files) j->phase++;
            if (j->phase == PH_DONE) {
                j->done = j->total;
                klog("install: finished successfully on disk %d", j->disk);
                return 1;
            }
        }
    } while (g_ticks - t0 < budget_ms);
    return 0;
}

int install_progress(const install_job_t *j) {
    if (!j || !j->total) return 0;
    return (int)((uint64_t)j->done * 1000 / j->total);
}

const char *install_phase(const install_job_t *j) { return j ? phase_names[j->phase] : ""; }
const char *install_error(const install_job_t *j) { return j && j->error[0] ? j->error : ""; }
uint32_t install_bytes_done(const install_job_t *j) { return j ? j->done * 512 : 0; }
uint32_t install_bytes_total(const install_job_t *j) { return j ? j->total * 512 : 0; }

void install_free(install_job_t *j) {
    if (!j) return;
    kfree(j->kernel);
    kfree(j->stage2);
    kfree(j->files);
    kfree(j->verify);
    kfree(j);
}
