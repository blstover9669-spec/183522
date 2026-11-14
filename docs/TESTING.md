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

# the app rectangle: click Files - dark icon with a spinner until its window is
# open, then the glowing outline with a light running round it; Alt+F4 fades it out
tools/shot.py --kernel build/kernel.elf --out shots ready click:560,744 move:300,650 shot:spinner \
    wait:1 shot:glow key:alt-f4 wait:0.15 shot:glow-fading

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
The Installer copies the current files without the system files, so the
drive's first boot is its first start: automated (the command line carries
`testmouse=1`), it writes the system files and the account "User" by itself
(`firststart: done ...`, `persist: saved ... sync`) and goes on to the desktop.
The installed system logs `persist: restored N bytes from slot A|B` at boot
and `persist: saved N bytes to slot ... (save #N, background|sync)` on every
save (background = a few seconds after a change or the 60 s timer, sync =
before reboot / shut down). On the drive OSaver and Installer are hidden
(desktop, Start menu, search: `apps: OSaver hidden (an ISO app, ...)`).

```sh
truncate -s 128M disk.img
# install (or drive the wizard with clicks: Next/Install are at about (753, 569))
tools/shot.py --kernel build/kernel.elf --hda disk.img --cmdline "autorun=Installer:auto" \
    --out shots-inst ready "serial:INSTALL: done"
# boot the disk alone (its first start), write a file, wait for the automatic save
tools/shot.py --disk disk.img --out shots-disk ready key:meta_l wait:0.5 type:terminal key:ret \
    wait:1.5 "type:echo hi > Desktop/proof.txt\n" "serial:background)" shot:saved
# next boot: the file is still there
tools/shot.py --disk disk.img --out shots-disk2 ready key:meta_l wait:0.5 type:terminal key:ret \
    wait:1.5 "type:cat Desktop/proof.txt\n" wait:0.5 shot:persisted
```

After a reboot inside one shot.py run the serial log keeps growing (the
first `ready` already matched), so wait with `serial:` for a later message,
and prefer the keyboard (`key:meta_l` opens the Start menu): the pointer
starts again at the top-left corner but shot.py still remembers the old
position.

### NovaOS Drive installation (ISO boots), first start, start screen

ISO boots show the NovaOS Drive installation screen after the splash; an
installed drive's first start shows its installation menu; every start shows the
start screen before the desktop. Automated starts (`autorun=`, `testmouse=` - so
every `--kernel` run) skip all three (a drive's first start then installs with
the defaults: account "User", no password - `firststart: done ... (automated
start ...)`). `setup=1` brings them all back (`setup=0` skips the first two),
`startscreen=1` / `startscreen=0` only the start screen. Both are **carried into
a drive installed from that start**, like `testmouse=` and `res=`. Test the real
path through GRUB with an ISO whose command line has `testmouse=1 setup=1`
(accurate clicks) and a disk; `bootsetup: ready`, `firststart: ready` and
`startscreen: ready` mean the screen waits for input.

* **NovaOS Drive installation** (1024x768): the size rows at x = 300, y = 424 +
  28 * i (100 MB ... 10 GB), Install at (660, 494), Continue ISO at (660, 554);
  while it installs (about 2 s in QEMU) Cancel is at (512, 148). The finished
  screen (`bootsetup: finished screen`) has How to start your drive at (419, 466)
  and Restart at (648, 466); with the guide open the tabs VM / USB are at (451,
  342) / (571, 342) and Restart at (512, 593). The drive gets only the boot
  loader and the kernel (`install: ... files 0 bytes`). Restart restarts the VM
  and the ISO's GRUB menu boots the disk by itself.
* **The drive's first start** (installation menu, 1024x768): page 1 "This system
  now supports" - Next at (797, 558). Page 2 "Your account" - Account name at
  (622, 243), the pictures at (418 + 54 * i, 324) (person, star, mountains,
  flower, planet, cat), Pictures at (780, 324) and then its pictures at (418 + 54
  * i, 378), Password at (622, 459), Confirm password at (622, 536), Back at
  (648, 651), Install at (797, 651). Page 3 "Installing NovaOS" runs by itself
  (`firststart: done in N ms`, `persist: saved ... sync`). Keys work too: Enter
  = Next, typing goes into the focused field, Enter moves to the next field and
  installs from the last, Tab / Shift+Tab, Esc = Back.
* **The start screen** (`startscreen: ready (Start button|password box)`): the
  Start button (or the password box) at (512, 321), the arrow in the password
  box at (655, 321); Enter starts. A wrong password logs `startscreen: wrong
  password`; `startscreen: opening the desktop` - then `autorun=` apps and the
  Welcome tour start.
* Add `res=800x600` to the ISO's command line for the 800x600 layouts (all of
  these work with the keyboard alone).

After a restart inside one shot.py run the pointer is back at (0, 0) while
shot.py remembers the old position: put `move:0,0` before the next click.

```sh
make BUILD=build-setup CMDLINE="testmouse=1 setup=1" GRUB_TIMEOUT=1
truncate -s 512M disk.img
# install, Restart, the first start's menu (name, picture, password), the start screen
tools/shot.py --iso build-setup/NovaOS.iso --hda disk.img --out shots-setup \
    "serial:bootsetup: ready" shot:setup click:300,508 click:660,494 wait:0.3 shot:progress \
    "serial:bootsetup: finished screen" shot:done click:648,466 "serial:firststart: ready" move:0,0 \
    shot:supports click:797,558 "serial:firststart: page 2" type:Alice click:526,324 click:622,459 \
    type:nova click:622,536 type:nova shot:account click:797,651 "serial:startscreen: ready" \
    shot:start "type:wrong\n" wait:0.3 shot:wrong "type:nova\n" "serial:startscreen: opening" ready
# the next start of the drive: no menu, the start screen asks for the password
tools/shot.py --disk disk.img --out shots-again "serial:startscreen: ready" shot:start "type:nova\n" ready
# Cancel while the bar runs: back on the setup screen, sectors 0, 1 and 2047 of the
# disk are zero and the next boot of the ISO shows the setup screen again
tools/shot.py --iso build-setup/NovaOS.iso --hda disk.img --out shots-cancel \
    "serial:bootsetup: ready" click:300,508 click:660,494 wait:0.6 click:512,148 \
    "serial:install: cancelled" wait:0.5 shot:back
# Continue ISO: the save file screen (OSaver), then the start screen (Guest)
tools/shot.py --iso build-setup/NovaOS.iso --out shots-iso "serial:bootsetup: ready" \
    click:660,554 "serial:osaver: Do you have a save file?" wait:1 shot:save-file-screen \
    key:right key:ret "serial:startscreen: ready" shot:start key:ret ready
# quick drive tests with --kernel: the menu and the start screen carried into the drive
tools/shot.py --kernel build/kernel.elf --hda disk.img --cmdline "autorun=Installer:auto startscreen=1" \
    "serial:startscreen: ready" key:ret "serial:INSTALL: done"
```

A plain release build (`make BUILD=dir WERROR=1`, no `testmouse`) shows the real
order: keys work everywhere (Tab Tab Enter = Continue ISO, Right Enter = No on
the save file screen, Enter = Start); the Welcome tour opens only after the start
screen (`welcome: NovaOS has opened - the tour follows`).

The system files are logged at startup (`sysfiles: wrote the system files ...`
on ISO boots, `ui: loaded 8 fonts, 2 themes, 6 wallpapers ...`).

### No drive found

Only when the installed drive is really missing: not found at startup, or not
answering while NovaOS runs (`nodrive: the drive did not answer (...), N of 2`,
a read every 3 s). Deleting folders never shows it any more (they are protected)
and ISO boots never do.

```sh
# an installed system whose drive is detached: boot=disk with no disk attached
tools/shot.py --kernel build/kernel.elf --cmdline "boot=disk diskid=1234abcd" --out shots-nd \
    "serial:nodrive: No drive found" wait:1 shot:nodrive
# the disk goes away while NovaOS runs (QEMU's drive_del): No drive found about 6 s later
tools/shot.py --disk disk.img --out shots-nd2 ready "monitor:drive_del ide0-hd0" \
    "serial:nodrive: No drive found" wait:1 shot:gone
# the installed disk on an AHCI (SATA) controller: GRUB still boots it through the
# BIOS, the kernel finds no IDE hard disk - No drive found
tools/shot.py --iso build/NovaOS.iso --out shots-cos "serial:nodrive: No drive found" wait:1 shot:cos \
    --extra -device ahci,id=ahci -drive id=d0,if=none,file=disk.img,format=raw \
    -device ide-hd,drive=d0,bus=ahci.0
```

### System requirement fail

Every start logs the two checks: `reqfail: COS:// drive ok (disk 0, ...)` (or
`COS:// is the CD (ISO boot)`) and `reqfail: N bundle apps found in
/system/apps`; a failure logs `reqfail: System requirement fail - REASON
(DETAIL)` and shows the screen. Make an installed disk with the build under
test and use a copy of it per case (the system files are protected, so
`tools/sysdamage.py` edits the drive's saved files in the disk image):

```sh
truncate -s 128M disk.img
tools/shot.py --kernel build/kernel.elf --hda disk.img --cmdline "autorun=Installer:auto" \
    ready "serial:INSTALL: done"
tools/shot.py --disk disk.img ready "serial:firststart: done"   # the drive's first start
# COS:// not readable: QEMU's blkdebug fails reads of slot A's header (its lba is in
# "persist: using disk 0 (...) at lba N" of a normal boot)
printf '[inject-error]\nevent = "read_aio"\nerrno = "5"\nsector = "12288"\n' > blk.cfg
tools/shot.py --iso build/NovaOS.iso --out shots-eio "serial:reqfail: System" wait:1 shot:eio \
    --extra -drive file=blkdebug:blk.cfg:disk.img,format=raw,if=ide,index=0
# bundle app: an app file missing from the drive
cp disk.img d1.img && tools/sysdamage.py d1.img /system/apps/Paint.app --remove
tools/shot.py --disk d1.img --out shots-app "serial:reqfail: System" wait:1 shot:app
```

### File protection and damaged system files

Apps can't change anything in `/system` and nobody can delete `/system`,
`/home` or `/home/user`: the Terminal prints "This is a protected system file",
Files shows it in its error box, the alert sound plays and the log has `vfs:
protected: OP PATH refused (APP)`. The SHA-256 manifest of the system files
(`/system/manifest.sha256`) is checked at every start of an installed drive
(`sysfiles: N system files match the manifest (SHA-256)`); a damaged file is a
kernel panic naming it (shot.py then exits non-zero because of the panic):

```sh
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Terminal" --out shots-prot ready wait:1.5 \
    "type:rm -r /system\n" "type:mv /system/boot/boot.cfg /tmp/b\n" "type:echo hi > /system/x.txt\n" \
    "type:rm -r /home\n" wait:0.5 shot:refused
# Files: select kernel.bin, Delete, Enter -> "1 item could not be deleted: This is a protected system file."
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Files:/system/boot" --out shots-fprot ready \
    wait:1.5 click:435,165 key:delete wait:0.5 key:ret wait:0.5 shot:refused
# a damaged byte in a system file inside an installed disk image -> kernel panic
cp disk.img d2.img && tools/sysdamage.py d2.img /system/ui/fonts/ui.font
tools/shot.py --disk d2.img --out shots-panic "serial:KERNEL PANIC" wait:1 shot:panic
grep -A1 "KERNEL PANIC" shots-panic/serial.log    # A system file is damaged: /system/ui/fonts/ui.font
# a missing (non-app) system file: "A system file is missing: /system/boot/mbr.bin"
cp disk.img d3.img && tools/sysdamage.py d3.img /system/boot/mbr.bin --remove
tools/shot.py --disk d3.img --out shots-missing "serial:KERNEL PANIC"
```

`tools/sysdamage.py disk.img --list` lists the files in the drive's newest save.

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

SafetyShield: `safetyshield_samples=1` writes harmless sample files that
match the detection rules to `/tmp/ss-samples/` (text with the marker
strings, two-byte "MZ"/"PK" headers, and the standard EICAR antivirus test
file - no real malware; never in the shipped system), plus
`safe_note.txt` and `article.txt` (mentions tools and commands) that must
stay Safe. A scan logs each threat as `[safetyshield] WARN: <level>:
<path> (rule: <rule>)` and shows the notification (`[notifyd] SafetyShield:
SafetyShield detected a threat in your system`); the Scan button of
Settings > SafetyShield is at about (414, 470) and "Recent scanned files"
at (400, 504). Expected: ransom_note.txt Severe; wipe.sh, miner.cfg,
dropper.ps1 Dangerous; invoice.pdf.exe Trojan; eicar.com, disable_av.bat
High; toolbar.js Moderate; photo.jpg, game.apk, verify.sh Risky;
safe_note.txt and article.txt Safe (11 threats):

```sh
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Settings:safetyshield safetyshield_samples=1" ready \
    click:414,470 wait:0.7 shot:scanning "serial:scan finished" shot:done click:400,504 wait:0.5 shot:recent
grep "safetyshield" shots/serial.log
```

Without the samples a full scan must find nothing (`scan finished: N files
in ... ms, 0 threats found`) - check it on a fresh boot and on an installed
drive. `safetyshield_test=N` still makes one test file count as a threat of
risk level N. `safetyshield_check_file(path)` (what the Web Browser calls
after a download) logs `queued check: <path>`, then `checked <path>: Safe`
or the `WARN` line.

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

### Web Browser

`autorun=Web_Browser:URL` opens a page at start (a path like
`/home/user/Desktop/x.html` opens as `file://`). Serve test pages from the
host as in the network section and open them at `http://10.0.2.2:PORT/...`;
the browser waits (up to 15 s) for DHCP before it shows *No internet*.
Every page logs `browser: STATUS URL (N bytes, type)` and, once laid out,
`browser: URL: N elements, M stylesheets, style X ms, layout Y ms, Z items,
heap K KB used` (watch the heap figure to spot leaks when you load pages
again and again); downloads log `browser: download NAME -> PATH`.

```sh
python3 -m http.server 8080 &          # in a directory with test pages
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Web_Browser:http://10.0.2.2:8080/page.html" \
    --out shots-web ready wait:4 shot:page key:pgdn wait:0.5 shot:down \
    key:ctrl-l "type:http://10.0.2.2:8080/other.html\n" wait:3 shot:other --extra -nic user,model=e1000
```

* In the 980x640 browser window at the default position the client area
  starts at screen (22, 55): Back / Forward / Reload / Home are at about
  (44, 108), (78, 108), (112, 108), (146, 108), the padlock at (179, 108),
  the address bar from x = 192, the star at (938, 108), the menu at
  (976, 108); the page starts at y = 157. On a certificate warning, *Continue
  anyway (not safe)* is at about (392, 407).
* Keys: Ctrl+L (address bar), Ctrl+T / Ctrl+W (tabs), Ctrl+F (find), Ctrl+=
  / Ctrl+- / Ctrl+0 (zoom), Ctrl+U (source), Ctrl+H (history), Ctrl+D
  (bookmark), Alt+Left / Alt+Right, F5, PgUp / PgDn / Space.
* `scroll:N` with a positive N scrolls **up** (QEMU sends wheel-up), so use
  `scroll:-5` to scroll a page down.
* NovaOS Search: `gcc -O2 -Wall -Wextra -DSEARCH_HOST -Iapps -o /tmp/search_test
  tools/search_test.c apps/search_text.c apps/search_index.c -lm && /tmp/search_test`
  runs the host tests of the words, page reader, index and ranking ("ALL
  PASSED"). In QEMU type a query into the address bar (Ctrl+L) and watch
  `search:` lines in the serial log (`search: indexed URL (N words, M links)`
  for each page read, `search: looking on the web for "..."` for a live
  search, `search: saved N pages`); a timer tick that takes over 40 ms logs
  `search: a timer tick took N ms`. Boot with `search_allow_bad_certs=1` when
  the test network intercepts TLS, and use `monitor:set_link e1000.0 off` to
  check that searches still work offline from the index.
* Real sites: mirror a page with its stylesheets and pictures on the host
  (`wget -p -k -E -U "Mozilla/5.0 (NovaOS 2.9; i686) NovaBrowser/2.9" URL`)
  and serve the copy, or browse the live site when the test machine has
  internet (a network that intercepts TLS gets the certificate warning first).

### HTML Viewer and Notepad code colours

Copy test files into the guest from the host: serve a directory with
`python3 -m http.server` (network section) and save each file with the
Terminal's `fetch URL FILE` (`mkdir -p Desktop/site` first), then open them:
`open Desktop/site/index.html` (HTML Viewer, the default for .html / .htm),
`open Desktop/hello.c` (Notepad), `open -w Notepad Desktop/site/index.html`.
`autorun=HTML_Viewer:/path/page.html` opens a page at start (no argument: the
empty window, File > Open... / Ctrl+O). Pages log the browser's line
`browser: file:///...: N elements, M stylesheets, ...` once laid out.

```sh
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Terminal" --out shots-hv ready \
    "serial:DHCP: address" "type:mkdir -p Desktop/site\n" \
    "type:fetch http://10.0.2.2:8080/index.html Desktop/site/index.html\n" wait:0.5 \
    "type:open Desktop/site/index.html\n" wait:2 shot:viewer --extra -nic user,model=e1000
```

* The 880x600 HTML Viewer window, when it is the first window (frame at
  (72, 40), client at (73, 72)): File at about (88, 85); Reload, Zoom out,
  Zoom in and Open in Web Browser at about (119, 118), (211, 118), (343, 118)
  and (861, 118); the page starts at y = 138 and the status bar (the link
  under the pointer) is the bottom 22 px. A second window cascades 30 px
  further right and down.
* Keys: PgUp / PgDn / Space / arrows / Home / End scroll, Ctrl+= / Ctrl+- /
  Ctrl+0 zoom, F5 reloads, Ctrl+O opens, Ctrl+W closes.
* Notepad colours .html .htm, .css, .js, .c .h, .cpp .hpp .cc files and names
  the language in its status bar ("C", "C++", "JavaScript", "CSS", "HTML";
  "Plain text" for everything else); check them with `dark=1` too. Files over
  2 MB open in the big-file viewer, which colours its rows the same way.

### App watchdog (Hang Test)

**Hang Test** is a hidden app that stops responding on purpose: `hangtest=1`
shows it on the desktop and in the Start menu, `test hang` in the Terminal or
`autorun=Hang_Test` opens it. Its 420x230 window, as the first window (frame at
(72, 40)), has **Freeze** at about (183, 136) (loops forever), **Busy 12 s** at
(183, 178) (works 12 s, then returns) and **Freeze in malloc loop** at (183, 220)
(allocates and frees forever - the switch must only happen outside the heap's
critical sections); the line under them counts the seconds of a window timer.
About 5 s after a click the window "Hang Test is not responding" opens in the
middle of the screen: at 1024x768 **Close the app** is at about (532, 419) and
**Wait** at (662, 419); at 800x600 (420, 335) and (550, 335). Each Close logs a
heap check (`watchdog: heap check after closing it: ok`).

```sh
# Freeze: the window after 5 s, the desktop goes on (Notepad, the Start menu), Close
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Hang_Test" --out shots-wd ready wait:1 \
    click:183,136 "serial:Hang Test is not responding" wait:0.5 shot:dialog \
    dclick:141,410 wait:1.5 "type:still typing" shot:notepad click:78,744 wait:0.5 \
    click:532,419 wait:1 shot:closed
# Busy 12 s + Wait: the app comes back by itself
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Hang_Test" --out shots-wd ready wait:1 \
    click:183,178 "serial:Hang Test is not responding" click:662,419 wait:1 shot:waiting \
    "serial:responds again" wait:1 shot:back
# Freeze in malloc loop, Wait (it runs in 20 ms slices next to the desktop), then Close
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Hang_Test" --out shots-wd ready wait:1 \
    click:183,220 "serial:Hang Test is not responding" click:662,419 wait:5 \
    click:78,744 wait:0.5 click:532,419 "serial:heap check after closing it"
```

Repeated hangs: with `hangtest=1` the Hang Test icon is at about (141, 45);
each new Hang Test window cascades 30 px further, so cycle k (from 0) clicks
Freeze at (183 + 30k, 136 + 30k) and waits for `(hang #N)` in the log. The
markers are listed in docs/API.md section 7a; a long normal operation (an
installation, a big copy, an 8 MB page) must not log `is not responding`.

### Test apps and hooks from NOVA CMD (`test`)

`test` in the Terminal lists the test apps people aren't normally supposed to
see and the kernel's test hooks, one line each; `test NAME` runs one:

| Name | What it does |
|---|---|
| `hang` | opens Hang Test (above) |
| `icons` | opens the hidden Icon Library (every system icon at every size) |
| `selftest` | runs the kernel self tests and prints `Kernel self tests: N passed, M failed` (like `selftest=1`; the `SELFTEST:` lines go to the log) |
| `fpu` | raises the handled x87 floating-point error (like `fputest=1`): NovaOS keeps running, `[fpu]` warning in the log |
| `shield` | writes the harmless SafetyShield samples to `/tmp/ss-samples` (like `safetyshield_samples=1`) and starts a scan |
| `panic` | asks "This stops NovaOS until you restart. Type yes to continue" in a window; `yes` shows the kernel panic screen (like `crashtest=1`) |
| `pz` | turns the puzzle test keys on until the next restart (like `pztest`): Breakout F11 / F12, Minesweeper F12 |
| `osaver-log` | OSaver writes its codes to the serial log (like `osaver_test=log`) |
| `osaver-prompt` | opens "Do you have a save file?" (like `osaver_test=promptN`) |
| `osaver-restore` | restores `~/Documents/OSaver code.txt` (like `osaver_test=restorefileN`) |

```sh
tools/shot.py --kernel build/kernel.elf --cmdline "autorun=Terminal" --out shots-test ready wait:1.5 \
    "type:test\n" wait:0.5 "type:test selftest\n" "serial:SELFTEST: done" wait:0.5 shot:tests
```

### Finding coordinates

* Screen is 1024x768. There is no taskbar: the bottom 48 px hold separate
  islands (y = 724..764): the Start button at about (28, 744); the running
  apps' buttons right of it, 40 px apart, the first at about (78, 744) (they
  shrink when many apps run; hovering shows the window title above); the app
  rectangle in the middle - Settings (464, 744), Welcome (512, 744), Files
  (560, 744); the clock box at the right (about (900, 744), click: calendar).
* Desktop icons sit in an 88x92 grid starting at (10, 10), column-major,
  sorted by app name; the icon image is centred at about (cell_x + 44,
  cell_y + 32). The games are not on the desktop (they are inside Games,
  whose icon is at about (54, 594)); About is first, at (53, 45).
* Games (`autorun=Games`, 592x240 client, `WIN_CENTER`): the game with index
  i in name order (2048, Breakout, Chess, Memory Match, Minesweeper, Pong,
  Snake, Solitaire, Tetris, Tic-Tac-Toe) has its icon at about
  (288 + 112 * (i % 5), 309 + 104 * (i / 5)), e.g. Tetris at (624, 413).
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
