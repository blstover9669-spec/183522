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
  persistence tests). Create one with `truncate -s 128M disk.img`.
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
clicking (the largest drive size that fits, or the whole disk when it is
smaller than 100 MB) and logs `INSTALL: done in N ms` (or `INSTALL: failed:
reason`).
The installed system logs `persist: restored N bytes from slot A|B` at boot
and `persist: saved N bytes to slot ... (save #N, background|sync)` on every
save (background = a few seconds after a change or the 60 s timer, sync =
before reboot / shut down).

```sh
truncate -s 128M disk.img
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

### NovaOS Drive installation (ISO boots) and No drive found

ISO boots show the NovaOS Drive installation screen after the splash, but
automated starts (`autorun=`, `testmouse=` - so every `--kernel` run) skip
it; `setup=1` brings it back. Test it through the real GRUB path, with an ISO
whose command line has `testmouse=1 setup=1` (accurate clicks) and a disk:
`bootsetup: ready` means it waits for input. At 1024x768 the size rows are at
x = 300, y = 424 + 28 * i (100 MB ... 10 GB), Install at (660, 494), Continue
ISO at (660, 554). Install restarts the VM; the ISO's GRUB menu then boots the
disk by itself (`boot: loader "NovaOS disk loader"` in the log).

```sh
make BUILD=build-setup CMDLINE="testmouse=1 setup=1" GRUB_TIMEOUT=1
truncate -s 512M disk.img
tools/shot.py --iso build-setup/NovaOS.iso --hda disk.img --out shots-setup \
    "serial:bootsetup: ready" shot:setup click:300,508 click:660,494 wait:0.3 shot:progress \
    "serial:bootsetup: drive installed" shot:done "serial:NovaOS disk loader" ready shot:installed
# Continue ISO: the save file screen (OSaver) comes next
tools/shot.py --iso build-setup/NovaOS.iso --out shots-iso "serial:bootsetup: ready" \
    click:660,554 ready wait:1 shot:save-file-screen
```

The system files are logged at startup (`sysfiles: wrote the system files ...`
on ISO boots, `ui: loaded 8 fonts, 2 themes, 6 wallpapers ...`). Deleting the
drive or a system file shows "No drive found" and logs `nodrive: No drive
found (PATH is missing)`, e.g. `"type:rm -r /system\n" "serial:nodrive:"`
in a Terminal (on an installed drive it stays like that after a restart, so
use a scratch disk).

### Power button, keyboard LEDs, empty IDE channels

`monitor:system_powerdown` presses the ACPI power button (what VirtualBox's
*ACPI Shutdown* does): NovaOS logs `power: ACPI power button pressed`, runs
the normal shut down (`persist: saved ... sync` on an installed system) and
QEMU exits when it powers off, so make it the last step. Keyboard LED updates
show up with QEMU's trace: `--extra -trace ps2_set_ledstate -D leds.log`.
VirtualBox reads 0x7F from an empty IDE channel; `ataempty=MASK` makes
positions read like that in QEMU (bit 0 primary master, 1 primary slave,
2 secondary master, 3 secondary slave):

```sh
tools/shot.py --iso build/NovaOS.iso ready monitor:system_powerdown "serial:power: shutting down"
tools/shot.py --kernel build/kernel.elf --cmdline "ataempty=15 autorun=Installer" ready \
    click:753,569 wait:0.8 shot:no-disks
```

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

The screensaver swallows the input that dismisses it, so mouse moves made
while it runs never reach the pointer and shot.py's idea of the pointer
position goes stale: dismiss it with a key (`key:esc`) when more clicks
follow. Settings > Styles (`autorun=Settings:styles`) picks the style
(rows at about (450, 258 + 40 * N) for style N = 0..4, Preview at (725,
462)); the serial log names it: `[screensaver] activated after N s idle
(Bubbles)`.

SafetyShield: `safetyshield_test=N` makes a test file count as a threat of
risk level N, so a scan logs `[safetyshield] WARN: ...` and shows the
notification (`[notifyd] SafetyShield: SafetyShield detected a threat in
your system`); the Scan button of Settings > SafetyShield is at about
(414, 468):

```sh
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Settings:safetyshield safetyshield_test=4" ready \
    click:414,468 wait:0.7 shot:scanning "serial:scan finished" shot:done click:400,504 wait:0.5 shot:recent
```

### Sound (recorded to a WAV file)

shot.py's QEMU has no sound card unless you add one, so by default NovaOS
uses the (silent) PC speaker. Add a card and QEMU's `wav` audio backend
with `--extra` and QEMU writes everything the guest plays to a WAV file
(only while the guest's stream runs, so leading silence is not recorded);
`tools/wavcheck.py` (numpy) lists the notes in it and `--expect` turns
that into a pass/fail check. The boot log names the output
(`audio: output Intel ICH AC'97, 48000 Hz, ...`); the Terminal's `sound`
command shows stream starts, underruns and the mixer's CPU share.

```sh
# AC'97: a beep from the Terminal, then check it is a 440 Hz tone
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Terminal" --out shots-snd ready wait:1 \
    "type:beep 440 1000\n" wait:2 --extra -audiodev wav,id=snd0,path=shots-snd/beep.wav \
    -device AC97,audiodev=snd0
tools/wavcheck.py shots-snd/beep.wav --expect 440
# Piano playing a song (Piano:2play-organ = second song, organ voice)
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Piano:play" --out shots-snd ready wait:8 \
    --extra -audiodev wav,id=snd0,path=shots-snd/song.wav -device AC97,audiodev=snd0
tools/wavcheck.py shots-snd/song.wav --expect 261.63,293.66,329.63,349.23,392
# chords: strongest frequencies of a time range ("sound test" plays C-E-G-C at ~0.7 s)
tools/wavcheck.py shots-snd/test.wav --peaks 0.7 1.5 --count 4
```

Sound Blaster 16: `-device sb16,audiodev=snd0` instead of the AC'97. The
PC speaker fallback can be recorded too: `-machine pcspk-audiodev=snd0`
without a card (QEMU then records only while the speaker is on). Force an
output with `sound=ac97|sb16|speaker` on the command line. A chord on the
Piano: `monitor:sendkey a-d-g 500` (QEMU 8.2 presses the keys one hold time
apart and releases them together, so C-E-G sound together from 1 s to 1.5 s
after the step: `--peaks 1.05 1.45` shows 261.6, 329.6 and 392 Hz at equal
level), or hold the mouse on a key with `monitor:mouse_button 1` while
`monitor:sendkey g 1200` plays another, then `monitor:mouse_button 0`.

### Network (e1000, DHCP, DNS, HTTP, HTTPS)

shot.py's QEMU has no network card unless you add one: `--extra -nic
user,model=e1000` gives NovaOS the same Intel PRO/1000 that VirtualBox uses,
behind QEMU's NAT (DHCP 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3). The host
machine is reachable from the guest at **10.0.2.2** (QEMU forwards it to the
host's 127.0.0.1), so a server started on the host can be fetched from
NovaOS. The network commands (`net`, `dhcp`, `ping`, `nslookup`, `fetch`,
`cookies` - docs/API.md section 23) log their results as `[net] ...` lines:

```sh
python3 -m http.server 8080 &                    # any directory
openssl req -x509 -newkey rsa:2048 -nodes -keyout k.pem -out c.pem -days 30 -subj /CN=10.0.2.2
openssl s_server -accept 8443 -cert c.pem -key k.pem -www -quiet &    # add -tls1_2, -groups P-256 ...
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Terminal" --out shots-net ready \
    "serial:DHCP: address" "type:fetch http://10.0.2.2:8080/\n" "serial:fetch: " \
    "type:fetch -k https://10.0.2.2:8443/\n" "serial:TLS 10.0.2.2" wait:1 shot:net \
    --extra -nic user,model=e1000
grep '\[net\]' shots-net/serial.log
```

Markers: `[net] link up`, `[net] DHCP: address 10.0.2.15/24, gateway ...`,
`[net] http: GET URL -> 200 OK, N bytes`, `[net] fetch: ... body sha256 HEX`
(compare with `sha256sum` on the host), `[net] TLS HOST: TLS 1.3,
AES_128_GCM_SHA256, X25519, certificate verified|NOT trusted`. A self-signed
test certificate is refused (`fetch` without `-k` logs "The certificate is
self-signed, so it is not trusted"), which is the expected result. Networks
that intercept TLS make real sites fail the certificate check the same way;
`fetch -k https://example.com/` still shows that the TLS connection itself
works. `selftest=1` runs the cryptography and certificate self tests
(`SELFTEST: crypto ...`, `SELFTEST: x509 chain verification`, `SELFTEST: tls
root certificates`).

### OSaver (save codes)

`osaver_test=log` logs small codes as `OSAVER-CODE:` lines, so a code made in
one boot can be typed into a fresh boot (`osaver_test=prompt` opens *Do you
have a save file?*; Yes is at about (440, 315), the text box below it). The
whole round trip, from a fresh ISO-like start each time:

```sh
# boot 1: change something, open OSaver, Start (Enter), log the code
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Terminal osaver_test=log" --out s1 ready \
    wait:1.5 "type:echo hi > Documents/test.txt\n" key:meta_l wait:0.5 type:osaver key:ret wait:1.5 \
    key:ret wait:2
grep OSAVER-CODE: s1/serial.log | sed 's/.*OSAVER-CODE: //' | tr -d '\n' > code.txt
# boot 2: Yes, type the code - the restore starts by itself
tools/shot.py --kernel build/kernel.elf --cmdline "osaver_test=prompt" --out s2 ready wait:1.5 \
    click:440,315 wait:0.5 "type:$(cat code.txt)" "serial:osaver: restore done" shot:restored
```

On the warning, Copy / View source with pretty print / View source with no
pretty print are at about (279, 372), (454, 372) and (687, 372). With
`setup=1` the real path runs: *Continue ISO* at about (659, 553), then the
prompt (`osaver: Do you have a save file?`). An invalid paste plays the alert
sound (record it with the AC'97 `--extra` above).

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
