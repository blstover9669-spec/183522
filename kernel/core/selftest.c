/*
 * selftest.c - boot-time self tests, enabled with the kernel option selftest=1.
 * Results go to the serial log as "SELFTEST: <name> PASS|FAIL" plus a summary line
 * "SELFTEST: done, N passed, M failed".
 */
#include "kernel.h"

static int passed, failed;

static void check(const char *name, bool ok) {
    if (ok) passed++;
    else failed++;
    klog("SELFTEST: %s %s", name, ok ? "PASS" : "FAIL");
}

static bool str_eq(const char *a, const char *b) {
    if (strcmp(a, b)) { klog("  got \"%s\" expected \"%s\"", a, b); return false; }
    return true;
}

static void test_printf(void) {
    char b[128];
    bool ok = true;
    snprintf(b, sizeof b, "%d|%i|%u|%x|%X|%5d|%-5d|%05d", -42, 7, 3000000000u, 0xbeef, 0xBEEF, 42, 42, 42);
    ok &= str_eq(b, "-42|7|3000000000|beef|BEEF|   42|42   |00042");
    snprintf(b, sizeof b, "%s|%10s|%-6s|%.2s|%c|%%", "hi", "right", "left", "truncate", 'Z');
    ok &= str_eq(b, "hi|     right|left  |tr|Z|%");
    snprintf(b, sizeof b, "%ld|%lu|%lld|%llu|%llx", -123456789L, 4000000000UL, -9000000000LL, 18000000000000000000ULL, 0x123456789abcULL);
    ok &= str_eq(b, "-123456789|4000000000|-9000000000|18000000000000000000|123456789abc");
    snprintf(b, sizeof b, "%f|%.2f|%.0f|%8.3f|%-8.1f|%e", 3.14159, 2.6751, 2.5, -1.5, 9.99, 12345.678);
    ok &= str_eq(b, "3.141590|2.68|3|  -1.500|10.0    |1.234568e+04");
    snprintf(b, sizeof b, "%g|%g|%g|%p", 0.0001, 123456.0, 1e-10, (void *)0x1234);
    ok &= str_eq(b, "0.0001|123456|1e-10|0x00001234");
    int n = snprintf(b, 5, "abcdefgh");
    ok &= n == 8 && str_eq(b, "abcd");
    check("snprintf", ok);
}

#define T(cond) do { if (!(cond)) { klog("  check failed: %s (line %d)", #cond, __LINE__); ok = false; } } while (0)

static void test_strings(void) {
    bool ok = true;
    char b[64];
    T(strlen("hello") == 5);
    T(strcmp("abc", "abd") < 0 && strcmp("b", "a") > 0 && !strcmp("x", "x"));
    T(strncasecmp("HeLLo", "hello world", 5) == 0);
    T(strstr("the quick fox", "quick") != NULL && !strstr("abc", "zz"));
    strlcpy(b, "truncate me please", 9);
    T(str_eq(b, "truncate"));
    T(atoi("  -1234x") == -1234 && strtol("0x1F", NULL, 0) == 31 && strtol("777", NULL, 8) == 511);
    T(strtoul("ff", NULL, 16) == 255);
    double d = strtod("-12.5e2", NULL);
    T(d == -1250.0);
    char tok[] = "a,b,,c";
    char *save, *t = strtok_r(tok, ",", &save);
    int cnt = 0;
    while (t) { cnt++; t = strtok_r(NULL, ",", &save); }
    T(cnt == 3);
    int x, y;
    char word[16];
    T(sscanf("12 34 hello", "%d %d %15s", &x, &y, word) == 3 && x == 12 && y == 34 && !strcmp(word, "hello"));
    float f;
    T(sscanf("3.5", "%f", &f) == 1 && f == 3.5f);
    memset(b, 0xAA, sizeof b);
    memmove(b + 1, b, 10);
    T((uint8_t)b[0] == 0xAA);
    strcpy(b, "0123456789");
    memmove(b + 2, b, 5);
    T(str_eq(b, "0101234789"));
    check("strings", ok);
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void test_qsort(void) {
    int a[500];
    for (int i = 0; i < 500; i++) a[i] = rand_range(-1000, 1000);
    qsort(a, 500, sizeof(int), cmp_int);
    bool ok = true;
    for (int i = 1; i < 500; i++) if (a[i - 1] > a[i]) ok = false;
    int key = a[250];
    ok &= bsearch(&key, a, 500, sizeof(int), cmp_int) != NULL;
    check("qsort/bsearch", ok);
}

static bool near(double a, double b) { return fabs(a - b) < 1e-9 * (1 + fabs(b)); }

static void test_math(void) {
    bool ok = true;
    ok &= near(sqrt(2.0), 1.4142135623730951);
    ok &= near(sin(M_PI / 6), 0.5) && near(cos(M_PI / 3), 0.5) && near(tan(M_PI / 4), 1.0);
    ok &= near(atan2(1, 1), M_PI / 4) && near(atan(1), M_PI / 4);
    ok &= near(pow(2, 10), 1024) && near(pow(9, 0.5), 3) && near(exp(1), M_E) && near(log(M_E), 1);
    ok &= near(log10(1000), 3) && floor(-2.5) == -3 && ceil(-2.5) == -2 && round(2.5) == 3;
    ok &= near(fmod(7.5, 2), 1.5) && fabs(-3.0) == 3.0 && trunc(-1.7) == -1;
    ok &= near(asin(1), M_PI / 2) && near(acos(0), M_PI / 2);
    check("libm", ok);
}

static void test_heap(void) {
    bool ok = true;
    void *ptrs[256];
    uint32_t sizes[256];
    memset(ptrs, 0, sizeof ptrs);
    uint32_t free_before = sys_mem_free();
    for (int round = 0; round < 4000 && ok; round++) {
        int i = rand_range(0, 255);
        if (ptrs[i]) {
            /* verify pattern then free or realloc */
            uint8_t *p = ptrs[i];
            for (uint32_t k = 0; k < sizes[i]; k += 97) if (p[k] != (uint8_t)(i + k)) { ok = false; break; }
            if (rand() & 1) { kfree(ptrs[i]); ptrs[i] = NULL; }
            else {
                uint32_t ns = (uint32_t)rand_range(1, 20000);
                uint8_t *np = krealloc(ptrs[i], ns);
                if (!np) { ok = false; break; }
                uint32_t keep = MIN(ns, sizes[i]);
                for (uint32_t k = 0; k < keep; k += 97) if (np[k] != (uint8_t)(i + k)) { ok = false; break; }
                for (uint32_t k = 0; k < ns; k++) np[k] = (uint8_t)(i + k);
                ptrs[i] = np;
                sizes[i] = ns;
            }
        } else {
            uint32_t sz = (rand() % 50 == 0) ? (uint32_t)rand_range(1 << 20, 6 << 20) : (uint32_t)rand_range(1, 5000);
            uint8_t *p = kmalloc(sz);
            if (!p) { ok = false; break; }
            if ((uintptr_t)p & 15) ok = false;
            for (uint32_t k = 0; k < sz; k++) p[k] = (uint8_t)(i + k);
            ptrs[i] = p;
            sizes[i] = sz;
        }
    }
    for (int i = 0; i < 256; i++) kfree(ptrs[i]);
    ok &= heap_check();
    ok &= sys_mem_free() == free_before;
    void *big = kmalloc(64u << 20);   /* 64 MiB */
    ok &= big != NULL;
    kfree(big);
    ok &= kcalloc(0x10000, 0x10000) == NULL;   /* overflow must fail */
    check("heap", ok);
}

static void test_vfs(void) {
    bool ok = true;
    ok &= vfs_mkdirs("/tmp/st/a/b") == 0 && vfs_is_dir("/tmp/st/a/b");
    ok &= vfs_write_file("/tmp/st/a/b/f.txt", "hello", 5) == 0;
    ok &= vfs_append_file("/tmp/st/a/b/f.txt", " world", 6) == 0;
    uint32_t sz;
    char *d = vfs_read_file("/tmp/st/a/../a/./b/f.txt", &sz);
    ok &= d && sz == 11 && !strcmp(d, "hello world");
    kfree(d);
    int fd = vfs_open("/tmp/st/x.bin", VFS_O_RDWR | VFS_O_CREATE);
    ok &= fd >= 0;
    ok &= vfs_write(fd, "abcdef", 6) == 6 && vfs_seek(fd, 2, VFS_SEEK_SET) == 2;
    char buf[8] = { 0 };
    ok &= vfs_read(fd, buf, 3) == 3 && !strcmp(buf, "cde");
    ok &= vfs_close(fd) == 0;
    ok &= vfs_rename("/tmp/st/x.bin", "/tmp/st/a/y.bin") == 0 && vfs_exists("/tmp/st/a/y.bin") && !vfs_exists("/tmp/st/x.bin");
    ok &= vfs_copy("/tmp/st/a", "/tmp/st/c") == 0 && vfs_exists("/tmp/st/c/b/f.txt");
    ok &= vfs_remove("/tmp/st/a") == VFS_ENOTEMPTY;
    ok &= vfs_rename("/tmp/st/a", "/tmp/st/a/b/z") == VFS_EBUSY;
    vfs_stat_t st;
    ok &= vfs_stat("/tmp/st/c/y.bin", &st) == 0 && st.size == 6 && !st.is_dir;
    vfs_dirent_t ents[8];
    ok &= vfs_list("/tmp/st/c", ents, 8) == 2 && ents[0].is_dir;   /* dirs first */
    ok &= vfs_remove_recursive("/tmp/st") == 0 && !vfs_exists("/tmp/st");
    char norm[VFS_PATH_MAX];
    ok &= vfs_normalize("/home/user", "../x/./y/..", norm) == 0 && !strcmp(norm, "/home/x");
    check("vfs", ok);
}

static void test_time(void) {
    datetime_t dt = { 2024, 2, 29, 13, 45, 10, 0 };
    uint32_t t = sys_datetime_to_time(&dt);
    datetime_t back;
    sys_time_to_datetime(t, &back);
    bool ok = t == 1709214310u && back.year == 2024 && back.month == 2 && back.day == 29 &&
              back.hour == 13 && back.minute == 45 && back.second == 10 && back.weekday == 4;
    check("time", ok);
}

void selftest_run(void) {
    klog("SELFTEST: starting");
    test_printf();
    test_strings();
    test_qsort();
    test_math();
    test_heap();
    test_vfs();
    test_time();
    klog("SELFTEST: done, %d passed, %d failed", passed, failed);
}
