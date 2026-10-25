/*
 * nova/disk.h - hard disks (ATA), persistence of the file system to disk, and
 * the hard-disk installer back end.
 *
 *   #include <nova/disk.h>       (not included by nova.h)
 *
 * Disks: the kernel probes the legacy IDE channels (primary 0x1F0, secondary
 * 0x170, master + slave) at boot. ATAPI devices (CD/DVD drives) are skipped,
 * so disk_get() only returns hard disks. Sectors are 512 bytes. All disk I/O
 * is PIO and synchronous: keep each call small (a few dozen sectors) when you
 * call it from an app callback, and spread big jobs over timer ticks.
 *
 * Persistence: when NovaOS was started from an installed hard disk (kernel
 * command line "boot=disk", written by the installer) the whole VFS is saved
 * to the disk and restored at the next boot. Saving happens automatically a few
 * seconds after files change, at least every 60 seconds while there are unsaved
 * changes, and before reboot / shut down from the GUI. Apps may force a save
 * with persist_save() (e.g. a terminal "sync" command). On a CD/ISO boot
 * persistence is off and everything stays in RAM.
 */
#ifndef NOVA_DISK_H
#define NOVA_DISK_H

#include <nova/types.h>

#define DISK_SECTOR_SIZE 512
#define DISK_MAX         4

/* error codes (negative) */
#define DISK_OK       0
#define DISK_EINVAL  -1    /* bad disk index / range            */
#define DISK_EIO     -2    /* device reported an error          */
#define DISK_ETIMEOUT -3   /* device did not respond in time    */
#define DISK_ENOMEM  -4
#define DISK_ENOSPC  -5    /* image does not fit                */
#define DISK_ECORRUPT -6   /* bad signature / checksum          */
#define DISK_ENODEV  -7    /* no such disk / not installed      */
#define DISK_EBUSY   -8

typedef struct disk_info {
    int      index;          /* 0 .. disk_count()-1                          */
    int      channel;        /* 0 = primary IDE, 1 = secondary IDE           */
    int      slave;          /* 0 = master, 1 = slave                        */
    char     model[41];      /* "VBOX HARDDISK", "QEMU HARDDISK"             */
    char     serial[21];
    uint64_t sectors;        /* capacity in 512-byte sectors                 */
    bool     lba48;          /* supports 48-bit LBA                          */
    const char *location;    /* "Primary master" ...                         */
} disk_info_t;

int                disk_count(void);
const disk_info_t *disk_get(int index);
/* Probe the IDE channels again (e.g. after the user attached a disk). Returns the count. */
int                disk_rescan(void);
/* Read / write `count` sectors starting at `lba`. Return DISK_OK or DISK_E*. */
int                disk_read(int disk, uint64_t lba, uint32_t count, void *buf);
int                disk_write(int disk, uint64_t lba, uint32_t count, const void *buf);
/* Flush the drive's write cache. */
int                disk_flush(int disk);
/* "64 MB", "1.5 GB" (binary units, like VirtualBox shows them). */
void               disk_format_size(uint64_t bytes, char *out, int n);
const char        *disk_strerror(int err);

/* ---- persistence ----------------------------------------------------------------- */
typedef struct persist_info {
    bool     active;          /* running from an installed disk: files are saved to it */
    int      disk;            /* disk index, -1 if not active                       */
    bool     dirty;           /* there are unsaved changes                          */
    bool     saving;          /* a background save is in progress                   */
    bool     loaded;          /* files were restored from disk at boot              */
    uint32_t last_save;       /* sys_time() of the last successful save, 0 = never  */
    uint32_t saves;           /* successful saves since boot                        */
    uint32_t capacity;        /* bytes available for the saved file system image    */
    uint32_t used;            /* size of the last saved / loaded image              */
    int      last_error;      /* DISK_OK or the error of the last attempt           */
} persist_info_t;

bool persist_active(void);
void persist_get_info(persist_info_t *out);
/* Save the whole VFS to disk now (synchronous, normally well under a second).
   Returns DISK_OK (also when nothing changed), DISK_ENODEV when persistence is
   off, or an error. */
int  persist_save(void);
/* Start a background save (written in slices from the main loop). */
void persist_save_async(void);

/* ---- installer back end -------------------------------------------------------------- */
/* Minimum disk size accepted by the installer. */
#define INSTALL_MIN_BYTES (24u * 1024 * 1024)

typedef struct install_job install_job_t;

/* Bytes of the NovaOS system image (boot loader + kernel). */
uint32_t       install_system_bytes(void);
/* 0 if `disk` can take an installation, otherwise DISK_E* and a reason in `why`. */
int            install_check_disk(int disk, char *why, int n);
/* True if the disk already carries a NovaOS installation. */
bool           install_disk_has_novaos(int disk);
/* True if this system was booted from that disk. */
bool           install_is_boot_disk(int disk);
/* Prepare an installation onto `disk` (erases it once install_step runs).
   copy_files: also store the current files as the installed system's files. */
install_job_t *install_begin(int disk, bool copy_files, char *err, int n);
/* The same, using only the first size_bytes of the disk (partition and file storage
   end there; 0 = the whole disk). */
install_job_t *install_begin_sized(int disk, uint64_t size_bytes, bool copy_files, char *err, int n);
/* The drive sizes offered to the user: 100 MB, 150 MB, 200 MB, 250 MB, 500 MB,
   700 MB, 1 GB, 5 GB, 10 GB (binary units, like VirtualBox). */
int            install_size_count(void);
uint64_t       install_size_bytes(int i);
const char    *install_size_label(int i);              /* "100 MB" ... "10 GB"  */
bool           install_size_fits(int disk, uint64_t bytes);   /* not bigger than the disk */
int            install_size_default(int disk);         /* largest that fits, -1 = none */
/* Do up to `budget_ms` of work. Returns 0 while running, 1 when finished,
   or a negative DISK_E* error (see install_error()). Call from a timer. */
int            install_step(install_job_t *job, uint32_t budget_ms);
int            install_progress(const install_job_t *job);     /* 0 .. 1000            */
const char    *install_phase(const install_job_t *job);        /* "Copying system..."  */
const char    *install_error(const install_job_t *job);
uint32_t       install_bytes_done(const install_job_t *job);
uint32_t       install_bytes_total(const install_job_t *job);
void           install_free(install_job_t *job);

#endif
