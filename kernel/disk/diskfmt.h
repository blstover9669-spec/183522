/*
 * diskfmt.h - on-disk layout of an installed NovaOS system (internal).
 *
 *   LBA 0            MBR: stage 1 boot code (bootloader/mbr.asm), disk signature,
 *                    partition table (one active partition, type 0x7F, from
 *                    LBA 2048 to the end of the disk), 0x55AA
 *   LBA 1            install header (disk_header_t below), read by stage 1/2
 *   LBA 2 ..         stage 2 loader (bootloader/stage2.asm), up to 62 sectors
 *   LBA 2048 ..      kernel: flat memory image of [__kernel_start, __data_end)
 *                    loaded at 1 MiB, entered at _start like a Multiboot kernel
 *   persist_lba ..   persistence area: two slots (A/B) of persist_slot_sectors,
 *                    each = slot header sector + vfs_serialize() image. Saves go
 *                    to the older slot and are committed by writing its header
 *                    last, so a power cut never destroys the previous save.
 *
 * Keep the header field offsets in sync with bootloader/mbr.asm and
 * bootloader/stage2.asm (HDR_* constants).
 */
#ifndef DISKFMT_H
#define DISKFMT_H

#include <stdint.h>

#define DISK_HDR_MAGIC0 0x41564F4Eu      /* "NOVA" */
#define DISK_HDR_MAGIC1 0x4B534944u      /* "DISK" */
#define DISK_HDR_VERSION 1

#define DISK_HDR_LBA      1
#define DISK_STAGE2_LBA   2
#define DISK_STAGE2_MAX   62
#define DISK_KERNEL_LBA   2048
#define DISK_PART_TYPE    0x7F
#define DISK_LOAD_ADDR    0x100000u

typedef struct __attribute__((packed)) disk_header {
    uint32_t magic0, magic1;       /*  0: "NOVADISK"                               */
    uint32_t version;              /*  8                                           */
    uint32_t header_size;          /* 12: sizeof(disk_header_t)                    */
    uint32_t stage2_lba;           /* 16  (stage 1 reads these two)                */
    uint32_t stage2_sectors;       /* 20                                           */
    uint32_t kernel_lba;           /* 24  (stage 2 reads these)                    */
    uint32_t kernel_sectors;       /* 28                                           */
    uint32_t kernel_load;          /* 32: physical load address (1 MiB)            */
    uint32_t kernel_entry;         /* 36: entry point (_start)                     */
    uint32_t kernel_bytes;         /* 40                                           */
    uint32_t persist_lba;          /* 44: first sector of slot A                   */
    uint32_t persist_slot_sectors; /* 48: sectors per slot (incl. its header)      */
    uint32_t persist_slots;        /* 52: 2                                        */
    uint32_t disk_id;              /* 56: random id, also in cmdline "diskid="     */
    uint32_t install_time;         /* 60: sys_time() at installation               */
    char     cmdline[256];         /* 64: kernel command line ("boot=disk ...")    */
    char     version_str[32];      /* 320: "NovaOS 1.5"                            */
    uint32_t kernel_crc;           /* 352: CRC32 of the kernel image               */
    uint8_t  reserved[508 - 356];
    uint32_t checksum;             /* 508: CRC32 of bytes 0..507                   */
} disk_header_t;

_Static_assert(sizeof(disk_header_t) == 512, "disk header must be one sector");

#define PERSIST_MAGIC0 0x41564F4Eu      /* "NOVA" */
#define PERSIST_MAGIC1 0x53524550u      /* "PERS" */
#define PERSIST_VERSION 1

typedef struct __attribute__((packed)) persist_header {
    uint32_t magic0, magic1;       /* "NOVAPERS"                                   */
    uint32_t version;
    uint32_t disk_id;              /* must match the disk header                   */
    uint32_t seq;                  /* save counter: the higher valid slot wins     */
    uint32_t length;               /* bytes of VFS image following this sector     */
    uint32_t data_crc;             /* CRC32 of the image                           */
    uint32_t save_time;            /* sys_time()                                   */
    uint8_t  reserved[512 - 36];
    uint32_t checksum;             /* CRC32 of bytes 0..507                        */
} persist_header_t;

_Static_assert(sizeof(persist_header_t) == 512, "persist header must be one sector");

uint32_t crc32_update(uint32_t crc, const void *data, uint32_t len);   /* start with 0 */
static inline uint32_t crc32_buf(const void *data, uint32_t len) { return crc32_update(0, data, len); }

/* shared between persist.c and install.c */
int  persist_find_disk(uint32_t disk_id, disk_header_t *hdr_out);   /* -> disk index or -1 */
bool persist_read_header(int disk, disk_header_t *hdr);             /* valid NovaOS header? */
int  persist_boot_disk(void);                                       /* disk we booted from, -1 */

#endif
