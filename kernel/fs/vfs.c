/*
 * vfs.c - in-memory hierarchical filesystem (ramfs) with file descriptors and
 * whole-tree (de)serialisation for persistence.
 */
#include "kernel.h"

typedef struct vnode {
    char name[VFS_NAME_MAX];
    bool is_dir;
    uint8_t *data;
    uint32_t size, cap;
    uint32_t mtime, ctime;
    struct vnode *parent, *children, *next;
} vnode_t;

static vnode_t *root;
static uint32_t generation;
static uint32_t last_change_tick;
static uint32_t synced_generation;
static vfs_sync_fn sync_hook;
static uint32_t sync_delay = 2000;

#define MAX_FDS 64
static struct { vnode_t *node; uint32_t pos; int flags; bool used; } fds[MAX_FDS];

static void touch(vnode_t *n) {
    if (n) n->mtime = sys_time();
    generation++;
    last_change_tick = g_ticks;
}

uint32_t vfs_generation(void) { return generation; }

/* ---- paths ----------------------------------------------------------------------- */
int vfs_normalize(const char *cwd, const char *path, char *out) {
    if (!path) return VFS_EINVAL;
    char tmp[VFS_PATH_MAX * 2];
    if (path[0] == '/' || !cwd || !*cwd) {
        if (path[0] == '/') strlcpy(tmp, path, sizeof tmp);
        else { tmp[0] = '/'; strlcpy(tmp + 1, path, sizeof tmp - 1); }
    } else {
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, path);
    }
    /* split into components, resolving . and .. */
    char *comps[64];
    int n = 0;
    char *save, *tok = strtok_r(tmp, "/", &save);
    while (tok) {
        if (!strcmp(tok, ".")) {}
        else if (!strcmp(tok, "..")) { if (n) n--; }
        else {
            if (n >= 64 || strlen(tok) >= VFS_NAME_MAX) return VFS_EINVAL;
            comps[n++] = tok;
        }
        tok = strtok_r(NULL, "/", &save);
    }
    size_t pos = 0;
    out[0] = '/';
    out[1] = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(comps[i]);
        if (pos + 1 + l >= VFS_PATH_MAX) return VFS_EINVAL;
        out[pos++] = '/';
        memcpy(out + pos, comps[i], l);
        pos += l;
        out[pos] = 0;
    }
    if (pos == 0) { out[0] = '/'; out[1] = 0; }
    return 0;
}

const char *vfs_basename(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void vfs_dirname(const char *path, char *out) {
    char norm[VFS_PATH_MAX];
    if (vfs_normalize(NULL, path, norm) != 0) { strcpy(out, "/"); return; }
    char *s = strrchr(norm, '/');
    if (!s || s == norm) { strcpy(out, "/"); return; }
    *s = 0;
    strlcpy(out, norm, VFS_PATH_MAX);
}

void vfs_join(const char *dir, const char *name, char *out) {
    char tmp[VFS_PATH_MAX * 2];
    snprintf(tmp, sizeof tmp, "%s/%s", dir, name);
    if (vfs_normalize(NULL, tmp, out) != 0) strlcpy(out, dir, VFS_PATH_MAX);
}

const char *vfs_extension(const char *path) {
    const char *b = vfs_basename(path);
    const char *d = strrchr(b, '.');
    return (d && d != b) ? d + 1 : "";
}

/* ---- nodes ------------------------------------------------------------------------ */
static vnode_t *node_new(const char *name, bool dir) {
    vnode_t *n = kcalloc(1, sizeof *n);
    if (!n) return NULL;
    strlcpy(n->name, name, sizeof n->name);
    n->is_dir = dir;
    n->mtime = n->ctime = sys_time();
    return n;
}

static void node_free(vnode_t *n) {
    vnode_t *c = n->children;
    while (c) { vnode_t *nx = c->next; node_free(c); c = nx; }
    for (int i = 0; i < MAX_FDS; i++) if (fds[i].used && fds[i].node == n) fds[i].used = false;
    kfree(n->data);
    kfree(n);
}

static vnode_t *child(vnode_t *dir, const char *name) {
    for (vnode_t *c = dir->children; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

static void attach(vnode_t *dir, vnode_t *n) {
    n->parent = dir;
    n->next = dir->children;
    dir->children = n;
}

static void detach(vnode_t *n) {
    vnode_t *p = n->parent;
    if (!p) return;
    vnode_t **pp = &p->children;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    n->parent = NULL;
    n->next = NULL;
}

static vnode_t *lookup(const char *path) {
    char norm[VFS_PATH_MAX];
    if (!root || vfs_normalize(NULL, path, norm) != 0) return NULL;
    vnode_t *n = root;
    char *save, *tok = strtok_r(norm, "/", &save);
    while (tok && n) {
        if (!n->is_dir) return NULL;
        n = child(n, tok);
        tok = strtok_r(NULL, "/", &save);
    }
    return n;
}

/* Find the parent directory of path and the final component name. */
static vnode_t *lookup_parent(const char *path, char *name_out) {
    char norm[VFS_PATH_MAX], dir[VFS_PATH_MAX];
    if (vfs_normalize(NULL, path, norm) != 0 || !strcmp(norm, "/")) return NULL;
    strlcpy(name_out, vfs_basename(norm), VFS_NAME_MAX);
    vfs_dirname(norm, dir);
    vnode_t *p = lookup(dir);
    return (p && p->is_dir) ? p : NULL;
}

static int ensure_cap(vnode_t *n, uint32_t need) {
    if (need <= n->cap) return 0;
    uint32_t nc = n->cap ? n->cap : 64;
    while (nc < need) nc = nc < 0x40000000u ? nc * 2 : need;
    uint8_t *nd = krealloc(n->data, nc);
    if (!nd) return VFS_ENOMEM;
    n->data = nd;
    n->cap = nc;
    return 0;
}

/* ---- public namespace API -------------------------------------------------------- */
int vfs_stat(const char *path, vfs_stat_t *st) {
    vnode_t *n = lookup(path);
    if (!n) return VFS_ENOENT;
    if (st) {
        st->size = n->is_dir ? 0 : n->size;
        st->is_dir = n->is_dir;
        st->mtime = n->mtime;
        st->ctime = n->ctime;
    }
    return 0;
}

bool vfs_exists(const char *path) { return lookup(path) != NULL; }
bool vfs_is_dir(const char *path) { vnode_t *n = lookup(path); return n && n->is_dir; }

int vfs_mkdir(const char *path) {
    char name[VFS_NAME_MAX];
    vnode_t *p = lookup_parent(path, name);
    if (!p) return VFS_ENOENT;
    if (!name[0]) return VFS_EINVAL;
    if (child(p, name)) return VFS_EEXIST;
    vnode_t *n = node_new(name, true);
    if (!n) return VFS_ENOMEM;
    attach(p, n);
    touch(p);
    return 0;
}

int vfs_mkdirs(const char *path) {
    char norm[VFS_PATH_MAX];
    if (vfs_normalize(NULL, path, norm) != 0) return VFS_EINVAL;
    char partial[VFS_PATH_MAX] = "";
    size_t pos = 0;
    char *save, *tok = strtok_r(norm, "/", &save);
    while (tok) {
        size_t l = strlen(tok);
        partial[pos++] = '/';
        memcpy(partial + pos, tok, l);
        pos += l;
        partial[pos] = 0;
        vnode_t *n = lookup(partial);
        if (!n) { int r = vfs_mkdir(partial); if (r) return r; }
        else if (!n->is_dir) return VFS_ENOTDIR;
        tok = strtok_r(NULL, "/", &save);
    }
    return 0;
}

int vfs_create(const char *path) {
    vnode_t *n = lookup(path);
    if (n) return n->is_dir ? VFS_EISDIR : 0;
    char name[VFS_NAME_MAX];
    vnode_t *p = lookup_parent(path, name);
    if (!p) return VFS_ENOENT;
    n = node_new(name, false);
    if (!n) return VFS_ENOMEM;
    attach(p, n);
    touch(p);
    return 0;
}

int vfs_remove(const char *path) {
    vnode_t *n = lookup(path);
    if (!n) return VFS_ENOENT;
    if (n == root) return VFS_EBUSY;
    if (n->is_dir && n->children) return VFS_ENOTEMPTY;
    vnode_t *p = n->parent;
    detach(n);
    node_free(n);
    touch(p);
    return 0;
}

int vfs_remove_recursive(const char *path) {
    vnode_t *n = lookup(path);
    if (!n) return VFS_ENOENT;
    if (n == root) return VFS_EBUSY;
    vnode_t *p = n->parent;
    detach(n);
    node_free(n);
    touch(p);
    return 0;
}

int vfs_rename(const char *from, const char *to) {
    vnode_t *n = lookup(from);
    if (!n) return VFS_ENOENT;
    if (n == root) return VFS_EBUSY;
    char name[VFS_NAME_MAX];
    vnode_t *np = lookup_parent(to, name);
    if (!np) return VFS_ENOENT;
    if (!name[0]) return VFS_EINVAL;
    for (vnode_t *a = np; a; a = a->parent) if (a == n) return VFS_EBUSY;   /* into itself */
    vnode_t *existing = child(np, name);
    if (existing == n) return 0;
    if (existing) {
        if (existing->is_dir) return VFS_EEXIST;
        if (n->is_dir) return VFS_EEXIST;
        detach(existing);
        node_free(existing);
    }
    vnode_t *op = n->parent;
    detach(n);
    strlcpy(n->name, name, sizeof n->name);
    attach(np, n);
    touch(op);
    touch(np);
    return 0;
}

static int copy_node(vnode_t *src, vnode_t *dstdir, const char *name) {
    if (child(dstdir, name)) return VFS_EEXIST;
    vnode_t *n = node_new(name, src->is_dir);
    if (!n) return VFS_ENOMEM;
    if (!src->is_dir && src->size) {
        n->data = kmalloc(src->size);
        if (!n->data) { kfree(n); return VFS_ENOMEM; }
        memcpy(n->data, src->data, src->size);
        n->size = n->cap = src->size;
    }
    attach(dstdir, n);
    for (vnode_t *c = src->children; c; c = c->next) {
        int r = copy_node(c, n, c->name);
        if (r) return r;
    }
    return 0;
}

int vfs_copy(const char *from, const char *to) {
    vnode_t *src = lookup(from);
    if (!src) return VFS_ENOENT;
    char name[VFS_NAME_MAX];
    vnode_t *dp = lookup_parent(to, name);
    if (!dp) return VFS_ENOENT;
    for (vnode_t *a = dp; a; a = a->parent) if (a == src) return VFS_EBUSY;
    vnode_t *ex = child(dp, name);
    if (ex) {
        if (ex->is_dir || src->is_dir) return VFS_EEXIST;
        detach(ex);
        node_free(ex);
    }
    int r = copy_node(src, dp, name);
    touch(dp);
    return r;
}

typedef struct { vfs_dirent_t e; } sort_ent_t;

static int dirent_cmp(const void *a, const void *b) {
    const vfs_dirent_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

int vfs_list(const char *path, vfs_dirent_t *out, int max) {
    vnode_t *d = lookup(path);
    if (!d) return VFS_ENOENT;
    if (!d->is_dir) return VFS_ENOTDIR;
    int total = 0;
    for (vnode_t *c = d->children; c; c = c->next) total++;
    if (!out || max <= 0) return total;
    vfs_dirent_t *all = kmalloc(sizeof(vfs_dirent_t) * (size_t)MAX(total, 1));
    if (!all) return VFS_ENOMEM;
    int i = 0;
    for (vnode_t *c = d->children; c; c = c->next, i++) {
        strlcpy(all[i].name, c->name, sizeof all[i].name);
        all[i].is_dir = c->is_dir;
        all[i].size = c->is_dir ? 0 : c->size;
        all[i].mtime = c->mtime;
    }
    qsort(all, (size_t)total, sizeof *all, dirent_cmp);
    memcpy(out, all, sizeof(vfs_dirent_t) * (size_t)MIN(total, max));
    kfree(all);
    return total;
}

/* ---- whole files ------------------------------------------------------------------- */
void *vfs_read_file(const char *path, uint32_t *size) {
    vnode_t *n = lookup(path);
    if (!n || n->is_dir) return NULL;
    char *buf = kmalloc(n->size + 1);
    if (!buf) return NULL;
    if (n->size) memcpy(buf, n->data, n->size);
    buf[n->size] = 0;
    if (size) *size = n->size;
    return buf;
}

int vfs_write_file(const char *path, const void *data, uint32_t size) {
    int r = vfs_create(path);
    if (r) return r;
    vnode_t *n = lookup(path);
    if (!n) return VFS_ENOENT;
    if (size > n->cap || n->cap > size * 4 + 4096) {
        uint8_t *nd = kmalloc(MAX(size, 1));
        if (!nd) return VFS_ENOMEM;
        kfree(n->data);
        n->data = nd;
        n->cap = MAX(size, 1);
    }
    if (size) memcpy(n->data, data, size);
    n->size = size;
    touch(n);
    return 0;
}

int vfs_append_file(const char *path, const void *data, uint32_t size) {
    int r = vfs_create(path);
    if (r) return r;
    vnode_t *n = lookup(path);
    if (!n) return VFS_ENOENT;
    if (ensure_cap(n, n->size + size)) return VFS_ENOMEM;
    memcpy(n->data + n->size, data, size);
    n->size += size;
    touch(n);
    return 0;
}

/* ---- descriptors ------------------------------------------------------------------- */
int vfs_open(const char *path, int flags) {
    vnode_t *n = lookup(path);
    if (!n) {
        if (!(flags & VFS_O_CREATE)) return VFS_ENOENT;
        int r = vfs_create(path);
        if (r) return r;
        n = lookup(path);
        if (!n) return VFS_ENOENT;
    }
    if (n->is_dir) return VFS_EISDIR;
    int fd = -1;
    for (int i = 0; i < MAX_FDS; i++) if (!fds[i].used) { fd = i; break; }
    if (fd < 0) return VFS_EMFILE;
    if ((flags & VFS_O_TRUNC) && (flags & VFS_O_WRITE)) { n->size = 0; touch(n); }
    fds[fd].used = true;
    fds[fd].node = n;
    fds[fd].flags = flags ? flags : VFS_O_READ;
    fds[fd].pos = (flags & VFS_O_APPEND) ? n->size : 0;
    return fd;
}

static bool fd_ok(int fd) { return fd >= 0 && fd < MAX_FDS && fds[fd].used; }

int vfs_read(int fd, void *buf, uint32_t len) {
    if (!fd_ok(fd)) return VFS_EBADF;
    vnode_t *n = fds[fd].node;
    if (fds[fd].pos >= n->size) return 0;
    uint32_t avail = n->size - fds[fd].pos;
    if (len > avail) len = avail;
    memcpy(buf, n->data + fds[fd].pos, len);
    fds[fd].pos += len;
    return (int)len;
}

int vfs_write(int fd, const void *buf, uint32_t len) {
    if (!fd_ok(fd)) return VFS_EBADF;
    if (!(fds[fd].flags & (VFS_O_WRITE | VFS_O_APPEND))) return VFS_EBADF;
    vnode_t *n = fds[fd].node;
    if (fds[fd].flags & VFS_O_APPEND) fds[fd].pos = n->size;
    uint32_t end = fds[fd].pos + len;
    if (ensure_cap(n, end)) return VFS_ENOMEM;
    if (fds[fd].pos > n->size) memset(n->data + n->size, 0, fds[fd].pos - n->size);
    memcpy(n->data + fds[fd].pos, buf, len);
    fds[fd].pos = end;
    if (end > n->size) n->size = end;
    touch(n);
    return (int)len;
}

int vfs_seek(int fd, int32_t off, int whence) {
    if (!fd_ok(fd)) return VFS_EBADF;
    int64_t base = whence == VFS_SEEK_CUR ? fds[fd].pos : whence == VFS_SEEK_END ? fds[fd].node->size : 0;
    int64_t np = base + off;
    if (np < 0) return VFS_EINVAL;
    fds[fd].pos = (uint32_t)np;
    return (int)np;
}

int vfs_tell(int fd) { return fd_ok(fd) ? (int)fds[fd].pos : VFS_EBADF; }

int vfs_close(int fd) {
    if (!fd_ok(fd)) return VFS_EBADF;
    fds[fd].used = false;
    return 0;
}

static uint32_t total_bytes(vnode_t *n) {
    uint32_t t = n->is_dir ? 0 : n->size;
    for (vnode_t *c = n->children; c; c = c->next) t += total_bytes(c);
    return t;
}
uint32_t vfs_total_bytes(void) { return root ? total_bytes(root) : 0; }

const char *vfs_strerror(int e) {
    switch (e) {
    case VFS_OK: return "Success";
    case VFS_ENOENT: return "No such file or directory";
    case VFS_EEXIST: return "File exists";
    case VFS_ENOTDIR: return "Not a directory";
    case VFS_EISDIR: return "Is a directory";
    case VFS_ENOTEMPTY: return "Directory not empty";
    case VFS_EINVAL: return "Invalid argument";
    case VFS_ENOMEM: return "Out of memory";
    case VFS_EBADF: return "Bad file descriptor";
    case VFS_EMFILE: return "Too many open files";
    case VFS_EBUSY: return "Resource busy";
    case VFS_ECORRUPT: return "Corrupt filesystem image";
    default: return "Unknown error";
    }
}

/* ---- serialisation ------------------------------------------------------------------
 * Format (little endian):
 *   "NVFS" | u32 version (1) | u32 node_count | u32 payload_checksum
 *   node records in pre-order:
 *     u32 parent_index (0xFFFFFFFF for the root) | u8 is_dir | u8 name_len | name
 *     | u32 mtime | u32 ctime | u32 size | size bytes of data
 */
#define VFS_MAGIC "NVFS"
#define VFS_VERSION 1

typedef struct { uint8_t *buf; uint32_t cap, pos; uint32_t count; bool overflow; } ser_t;

static void put_bytes(ser_t *s, const void *p, uint32_t n) {
    if (s->buf && s->pos + n <= s->cap) memcpy(s->buf + s->pos, p, n);
    else if (s->buf) s->overflow = true;
    s->pos += n;
}
static void put32(ser_t *s, uint32_t v) { put_bytes(s, &v, 4); }
static void put8(ser_t *s, uint8_t v) { put_bytes(s, &v, 1); }

static void ser_node(ser_t *s, vnode_t *n, uint32_t parent) {
    uint32_t me = s->count++;
    put32(s, parent);
    put8(s, n->is_dir);
    uint8_t nl = (uint8_t)strlen(n->name);
    put8(s, nl);
    put_bytes(s, n->name, nl);
    put32(s, n->mtime);
    put32(s, n->ctime);
    uint32_t sz = n->is_dir ? 0 : n->size;
    put32(s, sz);
    if (sz) put_bytes(s, n->data, sz);
    for (vnode_t *c = n->children; c; c = c->next) ser_node(s, c, me);
}

int32_t vfs_serialize(uint8_t *buf, uint32_t cap) {
    if (!root) return VFS_EINVAL;
    ser_t s = { buf, cap, 16, 0, false };
    ser_node(&s, root, 0xFFFFFFFFu);
    if (!buf) return (int32_t)s.pos;
    if (s.overflow || s.pos > cap) return VFS_ENOMEM;
    uint32_t sum = 0;
    for (uint32_t i = 16; i < s.pos; i++) sum = sum * 31 + buf[i];
    memcpy(buf, VFS_MAGIC, 4);
    uint32_t v = VFS_VERSION;
    memcpy(buf + 4, &v, 4);
    memcpy(buf + 8, &s.count, 4);
    memcpy(buf + 12, &sum, 4);
    return (int32_t)s.pos;
}

int vfs_deserialize(const uint8_t *buf, uint32_t size) {
    if (!buf || size < 16 || memcmp(buf, VFS_MAGIC, 4)) return VFS_ECORRUPT;
    uint32_t ver, count, sum;
    memcpy(&ver, buf + 4, 4);
    memcpy(&count, buf + 8, 4);
    memcpy(&sum, buf + 12, 4);
    if (ver != VFS_VERSION || count == 0 || count > 1000000) return VFS_ECORRUPT;
    uint32_t chk = 0;
    for (uint32_t i = 16; i < size; i++) chk = chk * 31 + buf[i];
    if (chk != sum) return VFS_ECORRUPT;
    vnode_t **nodes = kcalloc(count, sizeof(vnode_t *));
    if (!nodes) return VFS_ENOMEM;
    uint32_t pos = 16;
    int err = 0;
    uint32_t i;
    for (i = 0; i < count; i++) {
        if (pos + 6 > size) { err = VFS_ECORRUPT; break; }
        uint32_t parent;
        memcpy(&parent, buf + pos, 4);
        uint8_t is_dir = buf[pos + 4], nl = buf[pos + 5];
        pos += 6;
        if (pos + nl + 12 > size || nl >= VFS_NAME_MAX) { err = VFS_ECORRUPT; break; }
        char name[VFS_NAME_MAX];
        memcpy(name, buf + pos, nl);
        name[nl] = 0;
        pos += nl;
        uint32_t mt, ct, sz;
        memcpy(&mt, buf + pos, 4);
        memcpy(&ct, buf + pos + 4, 4);
        memcpy(&sz, buf + pos + 8, 4);
        pos += 12;
        if (sz > size - pos) { err = VFS_ECORRUPT; break; }
        vnode_t *n = node_new(name, is_dir);
        if (!n) { err = VFS_ENOMEM; break; }
        n->mtime = mt;
        n->ctime = ct;
        if (sz) {
            n->data = kmalloc(sz);
            if (!n->data) { kfree(n); err = VFS_ENOMEM; break; }
            memcpy(n->data, buf + pos, sz);
            n->size = n->cap = sz;
            pos += sz;
        }
        nodes[i] = n;
        if (i == 0) {
            if (parent != 0xFFFFFFFFu || !is_dir) { err = VFS_ECORRUPT; i++; break; }
        } else {
            if (parent >= i || !nodes[parent]->is_dir) { err = VFS_ECORRUPT; i++; break; }
            /* append to keep the original order */
            vnode_t *p = nodes[parent];
            n->parent = p;
            vnode_t **pp = &p->children;
            while (*pp) pp = &(*pp)->next;
            *pp = n;
        }
    }
    if (err) {
        if (nodes[0]) node_free(nodes[0]);
        else for (uint32_t k = 0; k < i; k++) if (nodes[k]) node_free(nodes[k]);
        kfree(nodes);
        return err;
    }
    /* swap in the new tree; open descriptors are invalidated */
    for (int k = 0; k < MAX_FDS; k++) fds[k].used = false;
    vnode_t *old = root;
    root = nodes[0];
    root->name[0] = 0;
    kfree(nodes);
    if (old) node_free(old);
    generation++;
    synced_generation = generation;
    return 0;
}

void vfs_set_sync_hook(vfs_sync_fn fn, uint32_t delay_ms) {
    sync_hook = fn;
    sync_delay = delay_ms;
    synced_generation = generation;
}

void vfs_tick(void) {
    if (!sync_hook || generation == synced_generation) return;
    if (g_ticks - last_change_tick < sync_delay) return;
    synced_generation = generation;
    sync_hook();
}

void vfs_init(void) {
    root = node_new("", true);
    if (!root) kpanic("vfs: cannot allocate root");
}

/* ---- self test of serialisation ------------------------------------------------------- */
static bool vfs_selftest(void) {
    int32_t need = vfs_serialize(NULL, 0);
    if (need <= 0) return false;
    uint8_t *buf = kmalloc((size_t)need);
    if (!buf) return false;
    int32_t got = vfs_serialize(buf, (uint32_t)need);
    bool ok = got == need;
    uint32_t before = vfs_total_bytes();
    if (ok) ok = vfs_deserialize(buf, (uint32_t)got) == 0;
    if (ok) ok = vfs_total_bytes() == before && vfs_exists("/home/user/README.txt");
    /* corrupt image must be rejected and leave the tree intact */
    if (ok) {
        buf[got / 2] ^= 0x5A;
        ok = vfs_deserialize(buf, (uint32_t)got) == VFS_ECORRUPT && vfs_exists("/home/user/README.txt");
    }
    kfree(buf);
    klog("vfs: serialise self-test %s (%d bytes)", ok ? "passed" : "FAILED", (int)need);
    return ok;
}

/* ---- default content ---------------------------------------------------------------------- */
static void put_text(const char *path, const char *text) { vfs_write_file(path, text, (uint32_t)strlen(text)); }

void vfs_populate_defaults(void) {
    vfs_mkdirs("/home/user/Desktop");
    vfs_mkdirs("/home/user/Documents");
    vfs_mkdirs("/home/user/Pictures");
    vfs_mkdirs("/home/user/Music");
    vfs_mkdirs("/home/user/Downloads");
    vfs_mkdirs("/system");
    vfs_mkdirs("/tmp");

    char buf[512];
    snprintf(buf, sizeof buf, "%s %s\nKernel %s\nBuilt %s %s\n", sys_version(), sys_edition(), sys_kernel_version(), __DATE__, __TIME__);
    put_text("/system/version.txt", buf);
    put_text("/system/motd.txt", "Welcome to NovaOS! Have a great day.\n");

    put_text("/home/user/README.txt",
             "Welcome to NovaOS!\n"
             "==================\n\n"
             "NovaOS is a small graphical operating system written from scratch\n"
             "in C and assembly. Everything you see - the kernel, the window\n"
             "manager, the widgets and the apps - runs in a single address space.\n\n"
             "Getting started\n"
             "---------------\n"
             " * Double-click an icon on the desktop to start an app.\n"
             " * Click the NovaOS button in the bottom-left corner (or press the\n"
             "   Windows key / Ctrl+Esc) to open the Start menu. Start typing to\n"
             "   search for an app.\n"
             " * Drag windows by their title bar. Drag a window to the top edge to\n"
             "   maximize it, or to the left/right edge to snap it.\n"
             " * Alt+Tab switches windows, Alt+F4 closes the active window.\n"
             " * PrintScreen (or Ctrl+Alt+S) saves a screenshot to ~/Pictures.\n\n"
             "Your files live in /home/user. Have fun!\n");
    put_text("/home/user/Documents/notes.txt",
             "Notes\n-----\n\n"
             "- Try the different wallpapers (right-click the desktop).\n"
             "- Dark mode is one click away in the desktop menu.\n"
             "- The clock in the taskbar opens a calendar.\n");
    put_text("/home/user/Documents/todo.txt",
             "TODO\n\n[x] Boot NovaOS\n[ ] Explore every app\n[ ] Write a poem in the text editor\n[ ] Beat the high score\n");
    put_text("/home/user/Documents/poem.txt",
             "The Little Kernel\n\n"
             "Born at one megabyte, it woke without a sound,\n"
             "it set the tables, mapped the ground,\n"
             "it counted ticks a thousand times a second,\n"
             "and drew a sky where windows beckoned.\n");
    put_text("/home/user/Desktop/Welcome.txt",
             "Hello there!\n\nThis file lives in /home/user/Desktop, so it shows up on your desktop.\n"
             "Double-click it to open it in the text editor.\n");

    /* sample pictures: rendered wallpapers as BMP files */
    const int iw = 320, ih = 200;
    surface_t *s = surface_create(iw, ih);
    if (s) {
        static const char *names[] = { "Nova.bmp", "Aurora.bmp", "Sunset.bmp", "Ocean.bmp" };
        for (int i = 0; i < 4; i++) {
            theme_render_wallpaper(s, i);
            size_t sz;
            void *bmp = image_encode_bmp(s, &sz);
            if (bmp) {
                char path[VFS_PATH_MAX];
                vfs_join("/home/user/Pictures", names[i], path);
                vfs_write_file(path, bmp, (uint32_t)sz);
                kfree(bmp);
            }
        }
        surface_destroy(s);
    }
    vfs_selftest();
    synced_generation = generation;
    klog("vfs: populated, %u bytes in files", vfs_total_bytes());
}
