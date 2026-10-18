/*
 * nova/vfs.h - in-memory hierarchical filesystem (ramfs).
 *
 * Paths are absolute, '/'-separated ("/home/user/notes.txt"); "." and ".."
 * components are resolved. Names are at most VFS_NAME_MAX-1 bytes.
 * Functions returning int give 0 (or a byte count) on success and a negative
 * VFS_E* code on error.
 *
 * The whole tree can be (de)serialised to a byte blob for persistence.
 */
#ifndef NOVA_VFS_H
#define NOVA_VFS_H

#include <nova/types.h>

#define VFS_NAME_MAX 64
#define VFS_PATH_MAX 256

/* error codes */
#define VFS_OK        0
#define VFS_ENOENT   -1   /* no such file or directory */
#define VFS_EEXIST   -2   /* already exists            */
#define VFS_ENOTDIR  -3   /* not a directory           */
#define VFS_EISDIR   -4   /* is a directory            */
#define VFS_ENOTEMPTY -5  /* directory not empty       */
#define VFS_EINVAL   -6   /* bad argument / path       */
#define VFS_ENOMEM   -7   /* out of memory             */
#define VFS_EBADF    -8   /* bad file descriptor       */
#define VFS_EMFILE   -9   /* too many open files       */
#define VFS_EBUSY    -10  /* in use / cannot move into itself */
#define VFS_ECORRUPT -11  /* bad serialised image      */

typedef struct vfs_stat {
    uint32_t size;       /* bytes (0 for directories)            */
    bool     is_dir;
    uint32_t mtime;      /* modification time, seconds since 1970 */
    uint32_t ctime;      /* creation time                        */
} vfs_stat_t;

typedef struct vfs_dirent {
    char     name[VFS_NAME_MAX];
    bool     is_dir;
    uint32_t size;
    uint32_t mtime;
} vfs_dirent_t;

/* ---- whole-file helpers -------------------------------------------------- */
/* Read a whole file into a new buffer (NUL-terminated for convenience; the
   terminator is not counted in *size). free() it. NULL on error. */
void *vfs_read_file(const char *path, uint32_t *size);
/* Create or overwrite a file (parent directory must exist). */
int   vfs_write_file(const char *path, const void *data, uint32_t size);
int   vfs_append_file(const char *path, const void *data, uint32_t size);

/* ---- namespace ------------------------------------------------------------ */
int   vfs_stat(const char *path, vfs_stat_t *st);
bool  vfs_exists(const char *path);
bool  vfs_is_dir(const char *path);
int   vfs_mkdir(const char *path);             /* parent must exist          */
int   vfs_mkdirs(const char *path);            /* like mkdir -p              */
int   vfs_remove(const char *path);            /* file or empty directory    */
int   vfs_remove_recursive(const char *path);  /* rm -r                      */
int   vfs_rename(const char *from, const char *to);   /* move / rename        */
int   vfs_copy(const char *from, const char *to);     /* files and directories (recursive) */
int   vfs_create(const char *path);            /* empty file if missing      */

/* List a directory: fills up to max entries (dirs first, then by name).
   Returns the total number of entries (may exceed max) or an error. */
int   vfs_list(const char *path, vfs_dirent_t *out, int max);

/* ---- file descriptors ----------------------------------------------------- */
#define VFS_O_READ   0x01
#define VFS_O_WRITE  0x02
#define VFS_O_RDWR   (VFS_O_READ | VFS_O_WRITE)
#define VFS_O_CREATE 0x04
#define VFS_O_TRUNC  0x08
#define VFS_O_APPEND 0x10

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

int   vfs_open(const char *path, int flags);   /* returns fd >= 0          */
int   vfs_read(int fd, void *buf, uint32_t n); /* bytes read, 0 at EOF     */
int   vfs_write(int fd, const void *buf, uint32_t n);
int   vfs_seek(int fd, int32_t offset, int whence);   /* new position     */
int   vfs_tell(int fd);
int   vfs_close(int fd);

/* ---- paths ------------------------------------------------------------------ */
/* Normalise `path` (relative paths are taken relative to `cwd`, which may be
   NULL for "/") into out (VFS_PATH_MAX). Returns 0 or VFS_EINVAL. */
int         vfs_normalize(const char *cwd, const char *path, char *out);
const char *vfs_basename(const char *path);          /* pointer into path   */
void        vfs_dirname(const char *path, char *out); /* "/a/b" -> "/a"      */
void        vfs_join(const char *dir, const char *name, char *out);
const char *vfs_extension(const char *path);          /* "txt" or ""         */

/* ---- misc --------------------------------------------------------------- */
/* Incremented on every modification; poll it to refresh views. */
uint32_t    vfs_generation(void);
uint32_t    vfs_total_bytes(void);     /* sum of all file sizes */
const char *vfs_strerror(int err);

/* ---- persistence ---------------------------------------------------------- */
/* Serialise the whole tree. With buf == NULL returns the size needed.
   Returns bytes written, or a negative error if cap is too small. */
int32_t vfs_serialize(uint8_t *buf, uint32_t cap);
/* Replace the whole tree with a serialised image. */
int     vfs_deserialize(const uint8_t *buf, uint32_t size);

/* Optional persistence hook: if set (e.g. by a disk driver), called from the
   main loop a short while after the tree changes ("autosave"). */
typedef void (*vfs_sync_fn)(void);
void    vfs_set_sync_hook(vfs_sync_fn fn, uint32_t delay_ms);

#endif
