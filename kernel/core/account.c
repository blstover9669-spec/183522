/*
 * account.c - the NovaOS account: name, profile picture, password (NovaOS 2.5).
 *
 * Installed drives keep the account in /system/config/account, saved to the
 * drive with all other files:
 *
 *   name=Alice
 *   picture=person | star | mountains | flower | planet | cat | file
 *   salt=<16 bytes, hex>        only when a password is set
 *   hash=<32 bytes, hex>        SHA-256 (kernel/crypto), stretched:
 *   rounds=4096                 h = SHA-256(salt | password), then
 *                               h = SHA-256(h | salt) rounds-1 more times
 *
 * The password itself is never stored. A picture chosen from Pictures is kept as
 * a 128x128 copy in /system/config/account.bmp, so the account keeps it even if
 * the original is deleted. A drive without the file (e.g. its first start was
 * automated) uses "User", the person icon and no password.
 *
 * ISO starts (the live system, nothing is saved) always use "Guest" with the
 * person icon and no password; nothing can be changed there.
 */
#include "kernel.h"
#include <nova/account.h>
#include <nova/disk.h>
#include "crypto/crypto.h"

#define ACC_FILE    "/system/config/account"
#define ACC_PIC     "/system/config/account.bmp"
#define ACC_ROUNDS  4096
#define PIC_SIZE    128

static const char *const pic_keys[ACCOUNT_PIC_BUILTIN] = { "person", "star", "mountains", "flower", "planet", "cat" };

static struct {
    uint32_t gen;               /* vfs_generation() the cache was read at (0 = never) */
    char name[ACCOUNT_NAME_MAX + 1];
    int picture;
    bool has_pw;
    uint8_t salt[16], hash[32];
    int rounds;
    uint32_t text_hash;         /* of the file as last read: a change bumps serial    */
    uint32_t serial;
} A;

bool account_is_guest(void) { return !persist_active(); }

/* ---- the file ------------------------------------------------------------------------ */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool unhex(const char *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int hi = hexval(s[2 * i]), lo = hi < 0 ? -1 : hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return s[2 * n] == 0;
}

static void tohex(const uint8_t *in, int n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[2 * n] = 0;
}

static void load(void) {
    uint32_t g = vfs_generation();
    if (A.gen && A.gen == g) return;
    A.gen = g ? g : 1;
    strlcpy(A.name, "User", sizeof A.name);
    A.picture = ACCOUNT_PIC_PERSON;
    A.has_pw = false;
    A.rounds = ACC_ROUNDS;
    char *text = vfs_read_file(ACC_FILE, NULL);
    uint32_t h = 2166136261u;
    for (const char *p = text; p && *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    if (h != A.text_hash) { A.text_hash = h; A.serial++; }
    if (!text) return;
    bool salt = false, hash = false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "name") && *v) strlcpy(A.name, v, sizeof A.name);
        else if (!strcmp(k, "picture")) {
            if (!strcmp(v, "file")) A.picture = ACCOUNT_PIC_FILE;
            for (int i = 0; i < ACCOUNT_PIC_BUILTIN; i++)
                if (!strcmp(v, pic_keys[i])) A.picture = i;
        } else if (!strcmp(k, "salt")) salt = strlen(v) == 32 && unhex(v, A.salt, 16);
        else if (!strcmp(k, "hash")) hash = strlen(v) == 64 && unhex(v, A.hash, 32);
        else if (!strcmp(k, "rounds")) A.rounds = CLAMP(atoi(v), 1, 1000000);
    }
    kfree(text);
    A.has_pw = salt && hash;
    if (A.picture == ACCOUNT_PIC_FILE && !vfs_exists(ACC_PIC)) A.picture = ACCOUNT_PIC_PERSON;
}

static int save(void) {
    char buf[512], salt[33], hash[65];
    int n = snprintf(buf, sizeof buf,
                     "# NovaOS account (the drive's installation menu and Settings > Account).\n"
                     "# The password is never stored: only a salted SHA-256 hash of it.\n"
                     "name=%s\npicture=%s\n",
                     A.name, A.picture == ACCOUNT_PIC_FILE ? "file" : pic_keys[CLAMP(A.picture, 0, ACCOUNT_PIC_BUILTIN - 1)]);
    if (A.has_pw) {
        tohex(A.salt, 16, salt);
        tohex(A.hash, 32, hash);
        n += snprintf(buf + n, sizeof buf - (size_t)n, "salt=%s\nhash=%s\nrounds=%d\n", salt, hash, A.rounds);
    }
    vfs_mkdirs("/system/config");
    int r = vfs_write_file(ACC_FILE, buf, (uint32_t)n);
    A.gen = 0;                         /* read it back: the change counter follows the file */
    load();
    A.serial++;
    return r < 0 ? ACCOUNT_EIO : ACCOUNT_OK;
}

/* ---- reading --------------------------------------------------------------------------- */
const char *account_name(void) {
    if (account_is_guest()) return "Guest";
    load();
    return A.name;
}

int account_picture(void) {
    if (account_is_guest()) return ACCOUNT_PIC_PERSON;
    load();
    return A.picture;
}

bool account_has_password(void) {
    if (account_is_guest()) return false;
    load();
    return A.has_pw;
}

uint32_t account_serial(void) {
    if (!account_is_guest()) load();
    return A.serial;
}

/* ---- the password ---------------------------------------------------------------------- */
static void pw_hash(const uint8_t salt[16], int rounds, const char *pw, uint8_t out[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, salt, 16);
    sha256_update(&c, pw, strlen(pw));
    sha256_final(&c, out);
    for (int i = 1; i < rounds; i++) {
        sha256_init(&c);
        sha256_update(&c, out, 32);
        sha256_update(&c, salt, 16);
        sha256_final(&c, out);
    }
    memset(&c, 0, sizeof c);
}

bool account_check_password(const char *password) {
    if (!account_has_password()) return true;
    if (!password) return false;
    uint8_t h[32];
    pw_hash(A.salt, A.rounds, password, h);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(h[i] ^ A.hash[i]);   /* constant time */
    memset(h, 0, sizeof h);
    return diff == 0;
}

int account_set_password(const char *current, const char *password) {
    if (account_is_guest()) return ACCOUNT_EGUEST;
    load();
    if (A.has_pw && !account_check_password(current ? current : "")) return ACCOUNT_EWRONG;
    if (password && strlen(password) > ACCOUNT_PASS_MAX) return ACCOUNT_EINVAL;
    if (!password || !*password) {
        A.has_pw = false;
        memset(A.salt, 0, sizeof A.salt);
        memset(A.hash, 0, sizeof A.hash);
        klog("account: password removed");
    } else {
        random_bytes(A.salt, sizeof A.salt);
        A.rounds = ACC_ROUNDS;
        pw_hash(A.salt, A.rounds, password, A.hash);
        A.has_pw = true;
        klog("account: password set (salted SHA-256, %d rounds)", A.rounds);
    }
    return save();
}

/* ---- name and picture ---------------------------------------------------------------- */
int account_set_name(const char *name) {
    if (account_is_guest()) return ACCOUNT_EGUEST;
    if (!name) return ACCOUNT_ENAME;
    while (*name == ' ') name++;
    char n[ACCOUNT_NAME_MAX + 1];
    strlcpy(n, name, sizeof n);
    if (strlen(name) > ACCOUNT_NAME_MAX) return ACCOUNT_ENAME;
    int len = (int)strlen(n);
    while (len && n[len - 1] == ' ') n[--len] = 0;
    if (!len) return ACCOUNT_ENAME;
    for (int i = 0; i < len; i++)
        if ((unsigned char)n[i] < 32 || n[i] == 127 || n[i] == '=') return ACCOUNT_ENAME;
    load();
    strlcpy(A.name, n, sizeof A.name);
    klog("account: name \"%s\"", A.name);
    return save();
}

int account_set_picture(int pic) {
    if (account_is_guest()) return ACCOUNT_EGUEST;
    if (pic < 0 || pic >= ACCOUNT_PIC_BUILTIN) return ACCOUNT_EINVAL;
    load();
    A.picture = pic;
    klog("account: picture %s", pic_keys[pic]);
    return save();
}

int account_set_picture_file(const char *path) {
    if (account_is_guest()) return ACCOUNT_EGUEST;
    uint32_t size = 0;
    void *data = path ? vfs_read_file(path, &size) : NULL;
    surface_t *img = data ? image_decode(data, size) : NULL;
    kfree(data);
    if (!img) return ACCOUNT_EPICTURE;
    /* a square from the middle, scaled to PIC_SIZE */
    int side = MIN(img->w, img->h);
    surface_t *sq = surface_create(PIC_SIZE, PIC_SIZE);
    if (sq) gfx_blit_scaled(sq, 0, 0, PIC_SIZE, PIC_SIZE, img, (img->w - side) / 2, (img->h - side) / 2, side, side, true);
    surface_destroy(img);
    size_t bl = 0;
    void *bmp = sq ? image_encode_bmp(sq, &bl) : NULL;
    surface_destroy(sq);
    if (!bmp) return ACCOUNT_EPICTURE;
    vfs_mkdirs("/system/config");
    int r = vfs_write_file(ACC_PIC, bmp, (uint32_t)bl);
    kfree(bmp);
    if (r < 0) return ACCOUNT_EIO;
    load();
    A.picture = ACCOUNT_PIC_FILE;
    klog("account: picture from %s", path);
    return save();
}

const char *account_strerror(int err) {
    switch (err) {
    case ACCOUNT_OK:       return "Done";
    case ACCOUNT_EGUEST:   return "The Guest account of the ISO can't be changed.";
    case ACCOUNT_ENAME:    return "Type an account name (up to 32 characters).";
    case ACCOUNT_EWRONG:   return "The current password is wrong.";
    case ACCOUNT_EPICTURE: return "This file could not be opened as a picture.";
    case ACCOUNT_EIO:      return "The account could not be saved.";
    default:               return "That did not work.";
    }
}

/* The picture copy (account.bmp), decoded once per change - for icons.c. */
surface_t *account_picture_image(void);
surface_t *account_picture_image(void) {
    static surface_t *img;
    static uint32_t img_serial = ~0u;
    uint32_t s = account_serial();
    if (img && img_serial == s) return img;
    surface_destroy(img);
    img = NULL;
    img_serial = s;
    uint32_t size = 0;
    void *data = vfs_read_file(ACC_PIC, &size);
    if (data) img = image_decode(data, size);
    kfree(data);
    return img;
}
