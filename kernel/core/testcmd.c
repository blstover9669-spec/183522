/*
 * testcmd.c - NOVA CMD's `test`: the test apps people aren't normally supposed to
 * see, and the kernel's test hooks, while NovaOS runs.
 *
 *   test          lists them, one line each
 *   test NAME     runs one
 *
 * The hidden test apps (Hang Test, Icon Library) stay off the desktop, the Start
 * menu and its search. The hooks do what their kernel command line options do at
 * boot (docs/API.md section 17); hooks that only make sense at boot (testmouse,
 * screensaver=, safetyshield_test=, setup=...) are not here.
 */
#include "kernel.h"
#include "core/watchdog.h"
#include <nova/osaver.h>
#include <nova/safetyshield.h>

void selftest_run(void);                         /* kernel/core/selftest.c        */
void selftest_result(int *npassed, int *nfailed);
void fpu_test_error(void);                       /* kernel/arch/cpu.c             */
void safetyshield_write_samples(void);           /* kernel/services/safetyshield.c */
bool cmdline_set(const char *key, const char *val);   /* kernel/core/bootinfo.c   */

#define OSAVER_TEST_FILE "/home/user/Documents/OSaver code.txt"

static int open_app(const char *name, cmd_writer_t *out) {
    const app_desc_t *a = app_find(name);
    if (!a) { cmd_printf(out, "test: %s is not part of this NovaOS\n", name); return 1; }
    app_launch(a, NULL);
    cmd_printf(out, "%s opened.\n", name);
    return 0;
}

static int t_hang(cmd_writer_t *out) { return open_app("Hang Test", out); }
static int t_icons(cmd_writer_t *out) { return open_app("Icon Library", out); }

static int t_selftest(cmd_writer_t *out) {
    preempt_disable();              /* kernel work: never suspended, not counted as the Terminal's */
    selftest_run();
    preempt_enable();
    int ok, bad;
    selftest_result(&ok, &bad);
    cmd_printf(out, "Kernel self tests: %d passed, %d failed.%s\n", ok, bad,
               bad ? " The system log has the failed ones (logs)." : "");
    return bad ? 1 : 0;
}

static int t_fpu(cmd_writer_t *out) {
    fpu_test_error();
    cmd_printf(out, "An x87 floating-point error was raised (1 / 0 with the exceptions unmasked).\n"
                    "NovaOS handled it and keeps running - the [fpu] warning is in the system log (logs).\n");
    return 0;
}

static int t_shield(cmd_writer_t *out) {
    safetyshield_write_samples();
    int r = safetyshield_scan();
    cmd_printf(out, "Harmless SafetyShield sample files written to /tmp/ss-samples.\n");
    if (r < 0) cmd_printf(out, "SafetyShield is off, so no scan: start it in Services, then run test shield again.\n");
    else cmd_printf(out, "A scan %s - its results are in Settings > SafetyShield.\n", r ? "is already running" : "started");
    return r < 0 ? 1 : 0;
}

static void panic_answer(const char *text, void *ctx) {
    (void)ctx;
    if (!text || strcasecmp(text, "yes")) { klog("test: kernel panic test cancelled"); return; }
    klog("test: kernel panic on purpose (test panic): dividing by zero");
    __asm__ volatile("xor %%ecx, %%ecx\n\tdiv %%ecx" : : : "eax", "ecx", "edx");
}

static int t_panic(cmd_writer_t *out) {
    if (!ui_input_box(NULL, "Kernel panic test", "This stops NovaOS until you restart. Type yes to continue",
                      "", panic_answer, NULL)) {
        cmd_printf(out, "test: could not open the question window\n");
        return 1;
    }
    cmd_printf(out, "Answer the question in the window (type yes to show the kernel panic screen).\n");
    return 0;
}

static int t_pz(cmd_writer_t *out) {
    cmdline_set("pztest", "1");
    cmd_printf(out, "Puzzle test keys on until the next restart: Breakout F11 drops one capsule of each kind,\n"
                    "F12 clears the board; Minesweeper F12 opens every safe cell.\n");
    return 0;
}

static int t_osaver_log(cmd_writer_t *out) {
    const char *t = sys_cmdline_get("osaver_test");
    char v[160];
    if (t && *t && !strstr(t, "log")) snprintf(v, sizeof v, "%s,log", t);
    else strlcpy(v, "log", sizeof v);
    cmdline_set("osaver_test", v);
    cmd_printf(out, "OSaver now writes its codes (up to 8 KB) and the saved files to the serial log as\n"
                    "OSAVER-CODE lines, until the next restart.\n");
    return 0;
}

static int t_osaver_prompt(cmd_writer_t *out) {
    osaver_boot_prompt(NULL, NULL);
    cmd_printf(out, "The \"Do you have a save file?\" screen is open.\n");
    return 0;
}

static int t_osaver_restore(cmd_writer_t *out) {
    if (!vfs_exists(OSAVER_TEST_FILE)) {
        cmd_printf(out, "test: there is no %s (save a code there with OSaver first)\n", OSAVER_TEST_FILE);
        return 1;
    }
    int r = osaver_restore_file(OSAVER_TEST_FILE, NULL, NULL);
    cmd_printf(out, r == 0 ? "Restoring %s...\n" : "test: the code in %s can't be restored\n", OSAVER_TEST_FILE);
    return r == 0 ? 0 : 1;
}

static const struct {
    const char *name, *what;
    int (*run)(cmd_writer_t *out);
} tests[] = {
    { "hang", "Hang Test: an app that stops responding (the app watchdog)", t_hang },
    { "icons", "Icon Library: every system icon at every size", t_icons },
    { "selftest", "the kernel self tests and their result (like selftest=1)", t_selftest },
    { "fpu", "a handled x87 floating-point error (like fputest=1)", t_fpu },
    { "shield", "harmless SafetyShield samples + a scan (safetyshield_samples=1)", t_shield },
    { "panic", "the kernel panic screen (like crashtest=1) - asks first", t_panic },
    { "pz", "test keys in Breakout and Minesweeper (like pztest)", t_pz },
    { "osaver-log", "OSaver writes its codes to the serial log (osaver_test=log)", t_osaver_log },
    { "osaver-prompt", "the \"Do you have a save file?\" screen (osaver_test=promptN)", t_osaver_prompt },
    { "osaver-restore", "restore ~/Documents/OSaver code.txt (osaver_test=restorefileN)", t_osaver_restore },
};

static int cmd_test(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2) {
        for (int i = 0; i < ARRAY_SIZE(tests); i++)
            cmd_printf(out, "%-15s %s\n", tests[i].name, tests[i].what);
        return 0;
    }
    for (int i = 0; i < ARRAY_SIZE(tests); i++)
        if (!strcasecmp(argv[1], tests[i].name)) {
            klog("test: %s", tests[i].name);
            return tests[i].run(out);
        }
    cmd_printf(out, "test: no test called %s (test alone lists them)\n", argv[1]);
    return 1;
}
REGISTER_COMMAND("test", "test [NAME] - hidden test apps and tests (test alone lists them)", cmd_test);
