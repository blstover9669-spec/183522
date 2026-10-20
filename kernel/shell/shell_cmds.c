/* shell_cmds.c - `open`, `assoc`, `pin`/`unpin` and `recent` shell commands. */
#include "shell/shell_internal.h"

static int cmd_open(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2) {
        cmd_puts(out, "usage: open PATH|APP [-w APP]   (-w / --with: open with that app; -d: Open With dialog)\n");
        return 2;
    }
    const char *with = NULL;
    bool dialog = false;
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-w") || !strcmp(argv[i], "--with")) && i + 1 < argc) { with = argv[++i]; continue; }
        if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--dialog")) { dialog = true; continue; }
    }
    if (with && !app_find(with)) { cmd_printf(out, "open: no app named '%s'\n", with); return 1; }
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-w") || !strcmp(argv[i], "--with")) && i + 1 < argc) { i++; continue; }
        if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--dialog")) continue;
        const char *arg = argv[i];
        char path[VFS_PATH_MAX];
        cmd_resolve_path(out, arg, path);
        bool exists = vfs_exists(path);
        if (!exists && !with) {
            const app_desc_t *a = app_find(arg);
            if (a) { app_launch(a, NULL); continue; }
            cmd_printf(out, "open: %s: no such file, folder or app\n", arg);
            status = 1;
            continue;
        }
        if (!exists) { cmd_printf(out, "open: %s: no such file or folder\n", arg); status = 1; continue; }
        bool ok;
        if (dialog) ok = shell_open_with_dialog(path) != NULL;
        else if (with) ok = shell_open_with(path, with);
        else ok = shell_open(path);
        if (!ok) { cmd_printf(out, "open: could not open %s\n", path); status = 1; }
    }
    return status;
}
REGISTER_COMMAND("open", "open PATH|APP [-w APP] - open a file or folder with its default app, or start an app", cmd_open);

static int cmd_assoc(int argc, char **argv, cmd_writer_t *out) {
    if (argc < 2) {
        const char *exts[128];
        int n = MIN(assoc_extensions(exts, 128), 128);
        cmd_printf(out, "%-8s %-16s %s\n", "TYPE", "OPENS WITH", "DESCRIPTION");
        for (int i = 0; i < n; i++) {
            char fake[32];
            snprintf(fake, sizeof fake, "x.%s", exts[i]);
            const app_desc_t *a = !strcmp(exts[i], "folder") ? assoc_default_app("/") : assoc_default_app(fake);
            const char *u = assoc_user_default(exts[i]);
            cmd_printf(out, ".%-7s %-16s %s%s\n", exts[i], a ? a->name : "(Open With)",
                       !strcmp(exts[i], "folder") ? "Folder" : filetype_description(fake), u ? "  [user choice]" : "");
        }
        cmd_puts(out, "\nusage: assoc EXT [APP | --reset]\n");
        return 0;
    }
    char ext[16];
    const char *e = argv[1][0] == '.' ? argv[1] + 1 : argv[1];
    strlcpy(ext, e, sizeof ext);
    char fake[32];
    snprintf(fake, sizeof fake, "x.%s", ext);
    const char *probe = !strcasecmp(ext, "folder") ? "/" : fake;
    if (argc == 2) {
        const app_desc_t *def = assoc_default_app(probe);
        cmd_printf(out, ".%s (%s) opens with: %s%s\n", ext, !strcasecmp(ext, "folder") ? "Folder" : filetype_description(fake),
                   def ? def->name : "nothing (Open With is shown)", assoc_user_default(ext) ? " [user choice]" : "");
        const app_desc_t *apps[32];
        int n = MIN(assoc_apps(probe, apps, 32), 32);
        if (n) {
            cmd_puts(out, "Apps that can open it:");
            for (int i = 0; i < n; i++) cmd_printf(out, "%s %s", i ? "," : "", apps[i]->name);
            cmd_puts(out, "\n");
        }
        return 0;
    }
    /* set: the app name may contain spaces */
    char app[64] = "";
    for (int i = 2; i < argc; i++) { if (i > 2) strlcat(app, " ", sizeof app); strlcat(app, argv[i], sizeof app); }
    if (!strcmp(app, "--reset") || !strcmp(app, "-") || !strcmp(app, "default")) {
        assoc_set_default(ext, NULL);
        const app_desc_t *def = assoc_default_app(probe);
        cmd_printf(out, ".%s reset: opens with %s\n", ext, def ? def->name : "nothing (Open With)");
        return 0;
    }
    if (!assoc_set_default(ext, app)) { cmd_printf(out, "assoc: no app named '%s'\n", app); return 1; }
    cmd_printf(out, ".%s files now open with %s\n", ext, app_find(app)->name);
    return 0;
}
REGISTER_COMMAND("assoc", "assoc [EXT [APP|--reset]] - show or change which app opens a file type", cmd_assoc);

static int pin_common(int argc, char **argv, cmd_writer_t *out, bool pin) {
    if (argc < 2) {
        int n = shell_pin_count();
        cmd_puts(out, "Pinned to the taskbar:");
        for (int i = 0; i < n; i++) cmd_printf(out, "%s %s", i ? "," : "", shell_pin_get(i));
        cmd_puts(out, n ? "\n" : " (nothing)\n");
        return 0;
    }
    char app[64] = "";
    for (int i = 1; i < argc; i++) { if (i > 1) strlcat(app, " ", sizeof app); strlcat(app, argv[i], sizeof app); }
    const app_desc_t *a = app_find(app);
    if (!a) { cmd_printf(out, "%s: no app named '%s'\n", argv[0], app); return 1; }
    shell_set_pinned(a->name, pin);
    cmd_printf(out, "%s %s the taskbar\n", a->name, pin ? "pinned to" : "unpinned from");
    return 0;
}
static int cmd_pin(int argc, char **argv, cmd_writer_t *out) { return pin_common(argc, argv, out, true); }
static int cmd_unpin(int argc, char **argv, cmd_writer_t *out) { return pin_common(argc, argv, out, false); }
REGISTER_COMMAND("pin", "pin [APP] - pin an app to the taskbar (no argument: list pins)", cmd_pin);
REGISTER_COMMAND("unpin", "unpin APP - remove an app from the taskbar", cmd_unpin);

static int cmd_recent(int argc, char **argv, cmd_writer_t *out) {
    if (argc > 1 && !strcmp(argv[1], "clear")) { shell_recent_clear(); cmd_puts(out, "Recent items cleared.\n"); return 0; }
    int n = shell_recent_count();
    if (!n) cmd_puts(out, "No recent items.\n");
    for (int i = 0; i < n; i++) {
        const char *it = shell_recent_get(i);
        if (!strncmp(it, "app:", 4)) cmd_printf(out, "  %s (app)\n", it + 4);
        else cmd_printf(out, "  %s\n", it);
    }
    return 0;
}
REGISTER_COMMAND("recent", "recent [clear] - list recently opened files and apps", cmd_recent);
