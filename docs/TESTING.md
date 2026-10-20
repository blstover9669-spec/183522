# Testing NovaOS

VirtualBox is the target, QEMU (`qemu-system-i386`) is the test machine.
`tools/shot.py` boots a build headless, drives it with keyboard and mouse,
saves PNG screenshots and the serial log. **Look at the screenshots** (they
are ordinary PNGs) - don't assume something renders correctly.

## Building

```sh
make                         # build/kernel.elf + build/NovaOS.iso (+ copy at ./NovaOS.iso)
make BUILD=build-myapp       # everything under build-myapp/ (use your own dir when
                             # several people/agents build at the same time)
make BUILD=build-myapp CMDLINE="autorun=Notepad testmouse=1" GRUB_TIMEOUT=0
                             # ISO with a baked-in kernel command line
make WERROR=1                # treat warnings as errors
make run                     # boot the ISO in a QEMU window (serial on stdout)
make run-kernel CMDLINE="autorun=About"   # QEMU -kernel fast path
make clean
```

## shot.py

```
tools/shot.py (--iso ISO | --kernel ELF [--boot iso] | --disk disk.img) [--cmdline "..."]
              [--out DIR] [--hda disk.img] [--mem 256] [--vga std] [--timeout 60] [-v]
              STEP... [--extra QEMU-ARGS...]
```

* `--kernel build/kernel.elf` boots with QEMU `-kernel` (fastest; no GRUB, the
  kernel programs the Bochs/BGA display itself). `--cmdline` is passed as the
  kernel command line and **`testmouse=1` is added automatically**.
* `--kernel ... --boot iso` wraps the kernel in a temporary GRUB ISO (timeout 0)
  with your command line - tests the real VBE framebuffer path that
  VirtualBox uses.
* `--iso NovaOS.iso` boots an ISO as built (add `testmouse=1` to its
  `CMDLINE` if you need accurate mouse clicks).
* `--hda disk.img` attaches a raw disk as IDE primary master (installer /
  persistence tests). Create one with `truncate -s 64M disk.img`.
* `--disk disk.img` boots an *installed* disk on its own (no CD, no
  `-kernel`, BIOS boots the hard disk). The kernel command line is the one
  the installer wrote (`boot=disk diskid=...`, plus `testmouse=1` if the
  installing system had it, which `--kernel` mode always adds).
* `--extra ...` (must come last) passes raw QEMU arguments, e.g. a second
  disk: `--extra -drive file=d2.img,format=raw,if=ide,index=1`.
* Output: `OUT/<name>.png` per `shot:` step and `OUT/serial.log`. The exit
  code is non-zero if a step failed (timeout, QEMU died) or the serial log
  contains a kernel panic.

Steps run in order:

| Step | Action |
|---|---|
| `ready` | wait for `NOVA: desktop ready` on serial (do this first) |
| `serial:TEXT` | wait until TEXT appears in the serial log (e.g. your own `klog` marker) |
| `wait:SECONDS` | sleep (fractions allowed) |
| `shot:NAME` | screenshot to `OUT/NAME.png` |
| `key:NAMES` | QEMU `sendkey` names separated by spaces: `ret esc tab spc backspace up down left right home end pgup pgdn delete f1 alt-f4 ctrl-a shift-tab meta_l print ctrl-alt-delete` (`meta_l` = Windows key, `print` = PrintScreen) |
| `type:TEXT` | type a string (`\n` = Enter) |
| `move:X,Y` | move the pointer to screen coordinates |
| `click:X,Y[,left\|right\|middle]` | move and click |
| `dclick:X,Y` | double-click |
| `drag:X1,Y1,X2,Y2` | press, move, release (window moves, resizes, sliders, painting) |
| `scroll:N` | mouse wheel, positive = down |
| `monitor:CMD` | raw QEMU monitor command, output printed (e.g. `monitor:info registers`) |

### Examples

```sh
# desktop + Start menu + About app, all with the mouse
tools/shot.py --kernel build/kernel.elf --out shots ready shot:desktop \
    click:28,744 wait:0.5 shot:startmenu key:esc \
    dclick:53,45 wait:1 shot:about

# launch your app at boot and test its keyboard handling
tools/shot.py --kernel build-notepad/kernel.elf --cmdline "autorun=Notepad" --out shots-notepad \
    ready wait:0.5 type:"Hello world\n" key:ctrl-a wait:0.3 shot:typed

# open a file with an app (autorun passes "App:argument")
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Notepad:/home/user/README.txt" ready shot:x

# the real boot path (GRUB + VBE), dark mode
tools/shot.py --kernel build/kernel.elf --boot iso --cmdline "dark=1" ready shot:dark

# kernel self tests
tools/shot.py --kernel build/kernel.elf --cmdline "selftest=1 nosplash" \
    "serial:SELFTEST: done" && grep SELFTEST shots/serial.log
```

### Installer and persistence

`autorun=Installer:auto` installs to the first suitable disk without
clicking and logs `INSTALL: done in N ms` (or `INSTALL: failed: reason`).
The installed system logs `persist: restored N bytes from slot A|B` at boot
and `persist: saved N bytes to slot ... (save #N, background|sync)` on every
save (background = a few seconds after a change or the 60 s timer, sync =
before reboot / shut down).

```sh
truncate -s 64M disk.img
# install (or drive the wizard with clicks: Next/Install are at about (753, 569))
tools/shot.py --kernel build/kernel.elf --hda disk.img --cmdline "autorun=Installer:auto" \
    --out shots-inst ready "serial:INSTALL: done"
# boot the disk alone, write a file, wait for the automatic save
tools/shot.py --disk disk.img --out shots-disk ready key:meta_l wait:0.5 type:terminal key:ret \
    wait:1.5 "type:echo hi > Desktop/proof.txt\n" "serial:persist: saved" shot:saved
# next boot: the file is still there
tools/shot.py --disk disk.img --out shots-disk2 ready key:meta_l wait:0.5 type:terminal key:ret \
    wait:1.5 "type:cat Desktop/proof.txt\n" wait:0.5 shot:persisted
```

After a reboot inside one shot.py run the serial log keeps growing (the
first `ready` already matched), so wait with `serial:` for a later message,
and prefer the keyboard (`key:meta_l` opens the Start menu): the pointer
starts again at the top-left corner but shot.py still remembers the old
position.

### Services, notifications, Run dialog

Every syslog line is mirrored to serial as `[tag] message`, so services can
be waited for with `serial:`: `services: N registered, M started`,
`[notifyd] Title: body` (every notification), `[screensaver] activated after
N s idle` / `dismissed`, `[alarmd] alarm N: message`, `[indexer] indexed N
files and folders`. `screensaver=SECONDS` shortens the idle timeout.
`key:meta_l-r` opens the Run dialog, which also runs registered commands:

```sh
tools/shot.py --kernel build/kernel.elf --cmdline "screensaver=4" --out shots-svc ready \
    key:meta_l-r wait:0.3 "type:notify Mail 3 new\n" wait:0.8 shot:toast \
    "serial:[screensaver] activated" wait:2 shot:saver move:500,400 wait:0.5 shot:back
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Services" ready wait:1 shot:services
```

### Finding coordinates

* Screen is 1024x768; the taskbar is the bottom 48 px (Start button at about
  (28, 744), clock at the right).
* Desktop icons sit in an 88x92 grid starting at (10, 10), column-major,
  sorted by app name; the icon image is centred at about (cell_x + 44,
  cell_y + 32).
* A window created with `WIN_CENTER` and client size `cw x ch` has its frame at
  `x = (1024 - (cw + 2)) / 2`, `y = (720 - (ch + 33)) / 2`; the client area
  starts 1 px right and 32 px below the frame's top-left corner. The close
  button is the 46 px wide area at the right end of the 32 px title bar.
* Without `WIN_CENTER`, windows cascade: the first at frame (72, 40), the next
  at (102, 70), and so on.
* When in doubt take a screenshot first and read the coordinates off it.

## Serial log

Everything logged with `klog()` (and `printf`) appears in `OUT/serial.log`
with an uptime prefix. Useful markers: `NOVA: desktop ready`,
`NOVA: autorun done`, `apps: launching <name>`, `wm: window N "title" WxH`,
`SELFTEST: ...`. A crash prints `*** KERNEL PANIC ***`, the registers and a
symbolised stack trace:

```
*** KERNEL PANIC ***
CPU exception 14: Page fault at EIP=0011f2a4
...
Stack trace:
  0011f2a4  notepad_paint+0x54
  00127f78  wm_run+0x3a8
```

## Manual testing

`make run` opens a QEMU window: click into it to use the mouse (Ctrl+Alt+G
releases it). For VirtualBox see the README.
