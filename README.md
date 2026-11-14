# NovaOS

**NovaOS 2.9 Developer Preview** - a hobby operating system with a modern
graphical desktop, written from scratch in C and assembly.

![NovaOS desktop with the Start menu open](docs/screenshots/start-menu.png)

NovaOS is a small 32-bit x86 operating system. Nothing in it comes from Linux,
BSD or any other OS: the kernel, drivers, window manager, widget toolkit,
font renderer, icons and all 34 apps are freestanding C and NASM written for
this project. It boots from a CD image (via GRUB) straight into a desktop with
windows, a Start menu, rounded "islands" along the bottom edge (Start button,
running apps, an app rectangle, clock), background services and apps, and runs in
**VirtualBox** and QEMU. The **Installer** app copies it onto a virtual hard
disk, which then boots NovaOS on its own and keeps your files across
restarts.

**Download:** [`NovaOS.iso`](NovaOS.iso) in this repository (about 10 MB).
See [RELEASE_NOTES.md](RELEASE_NOTES.md) for what is new in 2.9.

> **Developer Preview:** NovaOS is a hobby project and a technology preview.
> It has been tested in QEMU, which emulates the same hardware VirtualBox
> does, but **not yet on real VirtualBox or real PCs** - see
> [Known limitations](#known-limitations). Use it in a virtual machine.

## Contents

* [Screenshots](#screenshots)
* [Features](#features)
* [The apps](#the-apps)
* [Running NovaOS in VirtualBox (step by step)](#running-novaos-in-virtualbox-step-by-step)
* [Running in QEMU](#running-in-qemu)
* [Using NovaOS](#using-novaos)
* [Keyboard shortcuts](#keyboard-shortcuts)
* [Building from source](#building-from-source)
* [Known limitations](#known-limitations)
* [Repository layout](#repository-layout)
* [Built with Claude](#built-with-claude)

## Screenshots

Every app in NovaOS 1.5 (each one screenshotted in QEMU from the 1.5 release build; in 2.9 the ten games live inside the Games app):

![All 30 NovaOS apps](docs/screenshots/all-apps.png)

| | |
|---|---|
| ![Terminal](docs/screenshots/terminal.png) | ![Files](docs/screenshots/files.png) |
| **Terminal** - the NOVA CMD banner and `color check` | **Files** - file-type icons, default-app badges, places |
| ![Settings](docs/screenshots/settings.png) | ![Services](docs/screenshots/services.png) |
| **Settings** - light / dark mode, accent colours, wallpapers | **Services** - background services with status and logs |
| ![Solitaire](docs/screenshots/solitaire.png) | ![Chess](docs/screenshots/chess.png) |
| **Solitaire** - Klondike with drag and drop | **Chess** - full rules against the computer |
| ![Tetris](docs/screenshots/tetris.png) | ![Paint](docs/screenshots/paint.png) |
| **Tetris** | **Paint** - brushes, shapes, fill, palette, BMP files |
| ![Piano](docs/screenshots/piano.png) | ![Dark mode](docs/screenshots/desktop-dark.png) |
| **Piano** - playing a song with a piano roll | Dark theme with overlapping windows |
| ![Installer](docs/screenshots/installer.png) | ![Installer finished](docs/screenshots/installer-done.png) |
| **Installer** - choosing the hard disk | Installation finished |
| ![Persistence](docs/screenshots/persistence.png) | ![About](docs/screenshots/about.png) |
| Booted from the installed disk: a file written before the restart is still there | **About** |

## Features

**Desktop**

* VBE linear framebuffer, 1024x768 in 32-bit colour by default (800x600
  safe mode, 24-bit modes, Bochs/BGA fallback)
* Double-buffered compositor with dirty rectangles, anti-aliased fonts
  (Inter + DejaVu Sans Mono), rounded windows and soft drop shadows
* Window manager: move, resize, maximize, minimize, snap to screen edges,
  Alt+Tab, z-order, focus
* No taskbar: separate rounded islands at the bottom - the Start button, one
  icon button per running app (title on hover), the fixed app rectangle in
  the middle (Settings, Welcome, Files: a click darkens the icon and shows a
  spinner until the app is open, then its outline glows with a light running
  round it) and a live clock with a calendar; Start menu with categories,
  recent items and search over apps, files and commands; Run dialog (Win+R)
  with autocomplete
* Desktop icons for apps and files, desktop context menus (Open / Open With
  / Rename / Delete / Properties, New Folder / New Text File, wallpaper, dark
  mode), boot splash, a start screen at every start (the time and date slide
  in, the account name and picture, a password if one is set), a Welcome tour
  once the desktop has opened
* Light and dark themes, accent colours, procedural wallpapers
* Hand-drawn icon library (apps, folders, file types, devices, status icons)
  tuned for 16, 32 and 48 px - every icon is drawn by NovaOS code, no image
  files, no emoji
* Widget toolkit: buttons, labels, text boxes, checkboxes, radio buttons,
  sliders, progress bars, list boxes, scroll bars, dropdowns, tabs, menu bars,
  context menus, message / input / file dialogs, notifications
* File associations with user-changeable defaults, Open With and Properties
  dialogs
* Supports HTML (the built-in **HTML Viewer**), CSS, JavaScript, C and C++
  files: their own file icons and type names, and code colours in Notepad

**System**

* 32-bit x86 protected-mode kernel booted by GRUB (Multiboot 1) from the CD,
  or by NovaOS's own two-stage boot loader from an installed hard disk
* Background services run by a service manager (autostart, restart on
  failure, per-service logs, system log in `/system/log/syslog`):
  notifications, screensaver (starfield + aurora), alarms, autosave, file
  indexer, system monitor
* Heap allocator, in-memory hierarchical file system, libc / libm subset,
  kernel panic screen with symbolised stack traces
* Installer wizard and automatic saving of all files to the installed disk
  (two save slots, so a power cut never damages the last good save); a new
  drive starts with no system files and its first start is an installation
  menu (what NovaOS supports, your account, then it writes the system files)
* One account per drive (name, profile picture, optional password stored as a
  salted SHA-256 hash), changeable in Settings > Account; the ISO uses Guest
* File protection: apps can't change anything in `/system`, and `/system`,
  `/home` and `/home/user` can't be deleted; a SHA-256 manifest of the system
  files is checked at every start - a damaged system file is a kernel panic
* ACPI power off, ACPI power button (VirtualBox *ACPI Shutdown* shuts NovaOS
  down cleanly), reboot

**Drivers**

* PS/2 keyboard (US layout, Caps / Num / Scroll Lock LEDs) and PS/2 mouse
  with scroll wheel
* ATA PIO hard disks: IDE primary / secondary, master / slave, LBA28 / LBA48
* Sound: Intel ICH AC'97 and Sound Blaster 16 with an 8-voice software
  synthesizer (square, triangle, sine, saw, piano and organ voices); PC
  speaker fallback
* PCI enumeration, PIT timer, CMOS real-time clock, COM1 serial log

**Developer**

* Apps register themselves: drop a `.c` file into `apps/` and rebuild - see
  [docs/API.md](docs/API.md)
* `tools/shot.py` boots NovaOS headless in QEMU, drives it with keys and
  mouse and saves screenshots - see [docs/TESTING.md](docs/TESTING.md)

## The apps

34 apps. The ten games are only inside **Games**; every other app is in the
Start menu and on the desktop:

| Category | App | What it does |
|---|---|---|
| System | **Files** | Browse and manage files and folders: copy / cut / paste, rename, Open With, Properties |
| System | **Terminal** | NOVA CMD shell: over 70 commands, pipes, redirection, history, Tab completion, `color check` |
| System | **Settings** | Appearance (light / dark, accent, wallpaper), mouse, date and time, sound, account (name, picture, password), system information |
| System | **Services** | Start, stop and configure background services, read their logs |
| System | **Task Manager** | Open windows, installed apps, memory and CPU graphs |
| System | **Installer** | Install NovaOS on a hard disk (ISO only - hidden on an installed drive) |
| System | **OSaver** | Save your data (settings, app data, files, wallpaper) as a number code and load it back - on ISO boots *Do you have a save file?* asks for one (ISO only - hidden on an installed drive) |
| System | **Hex Viewer** | Inspect the raw bytes of any file |
| System | **About** | About NovaOS and this computer |
| System | **Welcome** | A seven-page tour of NovaOS (opens at first start) |
| Accessories | **Calculator** | Standard and scientific calculator with memory |
| Accessories | **Calendar** | Month calendar with notes for each day |
| Accessories | **Clock** | Analog clock, stopwatch and countdown timer |
| Accessories | **Notepad** | Text editor with find / replace, undo, zoom; code colours (keywords, strings, comments, numbers, tags) for HTML, CSS, JavaScript, C and C++ files |
| Accessories | **Sticky Notes** | Coloured notes that stay on your desktop |
| Accessories | **Typing Tutor** | Touch-typing practice with live WPM and accuracy |
| Internet | **Web Browser** | Tabs, address bar (a web address, or a search with NovaOS Search - the browser's own search engine with its own crawler, index and ranking, Browser://Search), back / forward / reload / home, padlock and certificate warnings, bookmarks bar, history, downloads into Downloads (listed on Browser://Downloads, Ctrl+J), find in page, zoom, page source; HTML and CSS pages (flexbox, grid, positioning), PNG / JPEG / GIF / SVG pictures and forms (no JavaScript) |
| Internet | **HTML Viewer** | Opens HTML files saved on this computer (.html, .htm - their default app): the page with its stylesheets and pictures, scrolling, links between local pages, Reload, Zoom in / out, Open in Web Browser, File > Open...; web links open in the Web Browser |
| Graphics | **Paint** | Draw and edit pictures (BMP) |
| Graphics | **Image Viewer** | View BMP (and PPM) pictures: zoom, fit, pan, folder browsing, slideshow |
| Graphics | **Fractal Explorer** | Explore the Mandelbrot and Julia sets |
| Multimedia | **Piano** | Play with the mouse or keyboard, chords and instruments on a sound card, built-in songs, recording |
| Development | **Widget Gallery** | Every UI toolkit widget in one window |
| Games | **Games** | All the games in one window (icon and name) - click one to play it |
| inside Games | **Solitaire** | Klondike: drag and drop, auto-play, hints, undo |
| inside Games | **Chess** | Full legal-move chess against the computer or a friend |
| inside Games | **Tetris** | Falling blocks with hold, ghost piece and levels |
| inside Games | **Minesweeper** | Beginner, intermediate and expert boards |
| inside Games | **2048** | Slide and merge the tiles |
| inside Games | **Snake** | Eat the apples, don't bite your tail |
| inside Games | **Breakout** | Smash the bricks with ball and paddle |
| inside Games | **Pong** | Beat the computer to 10 points |
| inside Games | **Memory Match** | Find the matching pairs |
| inside Games | **Tic-Tac-Toe** | Against an unbeatable computer or a friend |

## Running NovaOS in VirtualBox (step by step)

You need [VirtualBox](https://www.virtualbox.org/) 6.1 or 7.x and
[`NovaOS.iso`](NovaOS.iso).

### 1. Create the virtual machine

1. Click **New**. Name: `NovaOS`. Type: **Other**, Version:
   **Other/Unknown** (the 32-bit one, *not* 64-bit). Leave the ISO field empty
   for now (in VirtualBox 7 also tick *Skip Unattended Installation* if it
   appears).
2. **Memory: 256 MB** (64 MB is the minimum). One CPU is enough.
3. **Hard disk:** *Create a virtual hard disk now*, type **VDI**, dynamically
   allocated, size **1 GB** or more (100 MB is the minimum: the smallest NovaOS
   drive). You only need it if you want to install NovaOS, but it costs
   nothing to add it now.
4. Click **Finish**.

### 2. Settings

Select the VM and click **Settings**:

1. **System -> Motherboard:** Pointing Device **PS/2 Mouse** (not USB
   Tablet). Boot order: *Optical* first, then *Hard Disk*. Leave **Enable
   EFI unticked** (NovaOS boots via BIOS). Chipset PIIX3 is fine.
2. **Display -> Screen:** Graphics Controller **VBoxVGA** or **VMSVGA**,
   video memory 32 MB or more. Leave 3D acceleration off.
3. **Storage:** the hard disk must be on the **IDE controller** - NovaOS has
   no SATA/AHCI driver. If VirtualBox put the VDI under a *SATA* controller,
   select it, remove the attachment (the disk file is kept) and add it again
   under *Controller: IDE* (*Adds hard disk* -> *Choose existing disk*).
   Then click the empty optical drive under the IDE controller, click the
   disk icon on the right -> *Choose a disk file...* and pick **NovaOS.iso**.
4. **Audio:** tick *Enable Audio*, Host Audio Driver *Default*, Audio
   Controller **ICH AC97** (or **SoundBlaster 16**; *Intel HD Audio* is not
   supported). In VirtualBox 7 also tick *Enable Audio Output* (under
   *Extended Features*). VirtualBox does not emulate the PC speaker, so
   without one of these cards NovaOS is silent.
5. **Network:** keep *Enable Network Adapter* ticked, *Attached to:* **NAT**,
   and under *Advanced* set *Adapter Type* to **Intel PRO/1000 MT Desktop
   (82540EM)**. That is the only network card NovaOS has a driver for;
   VirtualBox picks a different card for "Other" machines, and then NovaOS
   has no internet.
6. Click **OK**.

### 3. Start NovaOS from the ISO

1. Click **Start**. The GRUB menu boots **NovaOS 2.9 Developer Preview**
   after 2 seconds and the splash screen plays.
2. **Click inside the VM window** to capture the mouse. Press **Right Ctrl**
   (VirtualBox's host key) to release it again. NovaOS has no VirtualBox
   Guest Additions, so the mouse is not shared automatically.
3. The **NovaOS Drive installation** screen follows: the hard disk in the
   middle, the storage sizes on the left (sizes bigger than the disk are
   greyed out), **Install** and **Continue ISO**.
   * **Continue ISO** runs NovaOS from the CD: everything works, but files
     stay in memory only and are gone after a restart. The save file screen
     (*Do you have a save file?* - see OSaver) comes first, then the start
     screen with the **Guest** account: click **Start** (or press Enter) to
     open the desktop.
   * **Install** - go on with step 4.

### 4. Install NovaOS on the hard disk

1. On the NovaOS Drive installation screen pick a storage size and click
   **Install**. All data on the disk is erased; only the chosen size of it is
   used. The drive gets only what it needs to boot (the boot loader and the
   kernel) - no system files and nothing from the ISO session. (Files live in memory while NovaOS runs, so the space you can really
   fill is limited by the VM's memory whatever size you pick.)
2. A progress bar shows the installation. **Cancel** under it stops the
   installation safely: the disk's boot record and drive signature are
   cleared, so nothing tries to start the half-written drive, and you are back
   on the NovaOS Drive installation screen.
3. When the drive is written: *Your drive has been installed and ready to have
   files savable*, with **How to start your drive** (a short guide with the
   tabs *VM* and *USB*) and **Restart**. Click **Restart**: the VM restarts
   **straight into the installed NovaOS** - the CD's boot menu finds the
   installed drive and boots it by itself after 2 seconds, so you can leave
   the ISO attached.
4. The drive's **first start is an installation**: after the splash a menu
   shows *This system now supports* (HTML with its new built-in app HTML
   Viewer, CSS, JS, C, C++) - **Next** - then *Your account*: the **Account
   name**, the **Profile picture** (the person icon, a built-in picture or a
   picture from Pictures) and an optional **Password** with **Confirm
   password** - **Install**. A progress bar writes the system files, the
   default folders and files and your account and saves them to the drive;
   then the start screen follows. (If the computer is switched off before
   that, the next start shows the menu again.)

You can also install from a running system: **Installer** on the desktop (or
Start menu -> System -> Installer) - choose the hard disk, the drive size and
click **Install**; *Copy my current files to the new system* takes the files
you have already made along (without the system files - that drive's first
start is the installation menu too).

### 5. Boot from the disk

1. With the ISO attached, the GRUB menu picks **Boot from the first hard disk
   (installed NovaOS)** by itself after 2 seconds (choose the NovaOS 2.9 entry
   within those 2 seconds to get the NovaOS Drive installation again). To
   start without the ISO: power off the VM, open *Settings -> Storage*, select
   the optical drive with NovaOS.iso, choose *Remove Disk from Virtual Drive*
   and start the VM - the BIOS then boots the hard disk directly. Keep the
   hard disk on the IDE controller. You briefly see `NovaOS loader / Loading
   NovaOS .... ok`, then the splash screen with a loading bar and the **start
   screen**: your account name and picture, with the time sliding in at the top
   left. Click **Start** (or press Enter) - with a password, type it in the box
   first (a wrong one is refused with the alert sound). Settings > Account
   changes the name, the picture and the password. OSaver and Installer are not
   shown on an installed drive.
2. Your files are now saved to the disk automatically: a few seconds after a
   change, at least every 60 seconds while there are unsaved changes, and
   before every restart or shut down. Shut down from the Start menu, or with
   VirtualBox's *Machine -> ACPI Shutdown*.
3. The drive also holds NovaOS itself: `/system/boot` (kernel, boot loader,
   boot settings), `/system/ui` (fonts, themes, wallpapers, UI settings,
   loaded at every start) and `/system/apps`. They are **protected**: apps
   (Terminal, Files, Notepad ...) can't change anything in `/system` ("This is
   a protected system file"), and `/system`, `/home` and `/home/user` can't be
   deleted. A SHA-256 manifest of the system files is checked at every start:
   if a system file is damaged (e.g. a broken disk sector) NovaOS stops with a
   **kernel panic** naming the file. Reinstall from the ISO (pick the NovaOS
   2.9 entry in the GRUB menu, then Install).
   NovaOS shows **No drive found** (black screen, Nova icon) when the installed
   drive is not there at startup, or stops responding while NovaOS runs.
4. NovaOS checks two more requirements at every start of the drive and shows
   **System requirement fail** (black screen, Nova icon, the reason under it)
   until you reset the VM:
   * *Requires a readable COS:// drive (at least 4 MB)* - COS:// is the drive
     NovaOS starts from. Reading it failed, or it is smaller than 4 MB. (When
     the installed hard disk is not found at all - for example it is on a
     SATA/AHCI, SCSI or NVMe controller: move it back to the IDE controller -
     the screen says **No drive found**.)
   * *A required bundle app is missing* - `/system/apps` holds one small file
     per app that comes with NovaOS; it or one of its files was deleted.
     Reinstall from the ISO.

### Troubleshooting

* **Black screen:** pick **NovaOS 2.9 Developer Preview (safe graphics
  800x600)** in the GRUB menu (press the arrow keys while it is shown). Installing from
  this entry makes the installed system use 800x600 too.
* **The pointer does not move:** the pointing device must be *PS/2 Mouse*,
  and you have to click into the VM window first.
* **No hard disk found / the Installer finds no disk / No drive found:** the
  disk is on a SATA, NVMe or SCSI controller - move it to the IDE controller
  (see step 2.3).
* **Forgot the password of the start screen:** NovaOS has no recovery - the
  drive has to be reinstalled from the ISO.
* **USB stick or a real PC:** the installed drive can't run from a USB stick
  (NovaOS only reads IDE hard disks). Writing NovaOS.iso to a stick with
  balenaEtcher or Rufus (DD mode) and booting a PC with legacy BIOS/CSM from it
  (boot menu key, often F12) runs NovaOS from the ISO, and nothing is saved.
  On a real computer choose **Continue ISO**, never Install - Install erases
  that computer's hard disk. NovaOS has only been tested in virtual machines.
* **All storage sizes are greyed out:** the virtual disk is smaller than
  100 MB. Give the VM a bigger disk.
* **No sound:** the audio controller must be *ICH AC97* or *SoundBlaster 16*
  with audio output enabled. `sound` in the Terminal shows which output
  NovaOS uses.
* **No internet ("No network adapter"):** set the VM's network *Adapter
  Type* to **Intel PRO/1000 MT Desktop** (Settings, step 5). `net` in the Terminal
  shows the network status.
* **PrintScreen does nothing:** your host computer kept the key (Windows 11
  opens its Snipping Tool with it). Press **Ctrl+Alt+S** instead - it saves the
  same screenshot to `/home/user/Pictures`.
* **Ctrl+Alt+Del** is caught by your host computer; send it with
  **Right Ctrl+Del** (host key + Del) to open Task Manager.
* **Closing the VM window:** choose *Send the shutdown signal* - NovaOS
  saves your files and powers off. *Power off the machine* is like pulling the
  plug: the last few seconds of changes may be lost (the previous save is
  never damaged).

### The same with VBoxManage

```sh
VBoxManage createvm --name NovaOS --ostype Other --register
VBoxManage modifyvm NovaOS --memory 256 --vram 32 --graphicscontroller vboxvga \
    --mouse ps2 --firmware bios --boot1 dvd --boot2 disk --boot3 none
VBoxManage modifyvm NovaOS --audio-driver default --audio-controller ac97 \
    --audio-enabled on --audio-out on      # VirtualBox 6.1: --audio default --audiocontroller ac97 --audioout on
VBoxManage modifyvm NovaOS --nic1 nat --nictype1 82540EM   # Intel PRO/1000 MT Desktop
VBoxManage storagectl NovaOS --name IDE --add ide --controller PIIX4
VBoxManage createmedium disk --filename "$HOME/NovaOS-disk.vdi" --size 1024 --format VDI
VBoxManage storageattach NovaOS --storagectl IDE --port 0 --device 0 \
    --type hdd --medium "$HOME/NovaOS-disk.vdi"
VBoxManage storageattach NovaOS --storagectl IDE --port 1 --device 0 \
    --type dvddrive --medium "$PWD/NovaOS.iso"
VBoxManage startvm NovaOS            # NovaOS Drive installation: pick a size, Install,
                                     # then Restart (boots the installed system)
```

## Running in QEMU

```sh
qemu-system-i386 -m 256 -vga std -cdrom NovaOS.iso                      # live CD
truncate -s 1G disk.img
qemu-system-i386 -m 256 -vga std -cdrom NovaOS.iso -hda disk.img -boot d  # install, Restart (then it boots the disk)
qemu-system-i386 -m 256 -vga std -hda disk.img -boot c                    # installed system
```

For sound add a card, e.g. `-audiodev pa,id=snd0 -device AC97,audiodev=snd0`
(PulseAudio; use `alsa`, `coreaudio` or `dsound` on other hosts, or
`-device sb16,audiodev=snd0` for a Sound Blaster 16). In the QEMU window, Ctrl+Alt+G releases the
mouse.

## Using NovaOS

* Double-click desktop icons, or open the **Start menu** (bottom-left button,
  Windows key or Ctrl+Esc) and type to search apps, files and commands.
  The games are in **Games**.
* The rectangle in the middle of the bottom edge opens Settings, Welcome and
  Files with one click. Right of the Start button there is a button for each
  running app: click it to bring the app to the front or minimize it,
  hover for its title, right-click for a menu.
* Drag windows by the title bar; drag to the top edge to maximize or to the
  left / right edge to snap.
* Right-click the desktop for new folders / text files, wallpapers and dark
  mode; right-click a file to open it with another app, rename, delete it or
  see its properties. The clock (bottom right) opens a calendar; Settings >
  Sound turns all sound on and off.
* **Files** opens files with their default app; change the app for a file
  type in *Properties -> Change...* or with `assoc txt Notepad` in the
  Terminal.
* The **Terminal** (`help` lists every command) has file, text and system
  commands, pipes and redirection, Tab completion, `color check` (display
  mode), `sync`, `df` and `disks` (installed system), `service`, `notify`,
  `alarm +5 Tea`, `logs`, `sound`, `open`, and a few toys (`neofetch`,
  `cowsay`, `matrix`).
* **Sound** goes to the AC'97 or Sound Blaster 16 card, else the PC speaker.
  `sound test` plays an arpeggio and a chord, `sound volume 0..100` sets the
  volume. Piano plays chords and has an instrument choice on a sound card.
* **Settings -> About** and the **About** app show the version, memory and
  display mode; **Settings -> Account** changes your account name, profile
  picture and password (on the ISO the account is Guest and can't change).

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Windows key, Ctrl+Esc | Open / close the Start menu (then type to search) |
| Win+R | Run dialog: open an app, file, folder or command by name |
| Alt+Tab | Switch windows |
| Alt+F4 | Close the active window |
| Ctrl+Alt+Del (VirtualBox: Right Ctrl+Del) | Task Manager |
| PrintScreen (also with Alt, Ctrl or Shift), Ctrl+Alt+S | Save a screenshot to `/home/user/Pictures` |
| Esc | Close menus and dialogs |
| Ctrl+N / O / S / W | New / open / save / close in Notepad and Paint |
| Ctrl+Z / Ctrl+Y | Undo / redo (Notepad, Paint) |
| Ctrl+X / C / V, F2, Del, F5 | Cut / copy / paste, rename, delete, refresh in Files |
| Ctrl+F / Ctrl+H / Ctrl+G | Find / replace / go to line in Notepad |
| Tab, Up / Down, Ctrl+C, Ctrl+L, Shift+PgUp / PgDn | Terminal: complete, history, interrupt, clear, scroll |
| Ctrl+Shift+C / Ctrl+Shift+V | Terminal: copy / paste |
| F2 | New game in most games |
| Right Ctrl (VirtualBox host key) | Release the mouse from the VM window |

## Building from source

Requirements (Debian / Ubuntu): `gcc` with 32-bit support (`gcc-multilib`),
`nasm`, `make`, `grub-pc-bin`, `grub-common` (for `grub-mkrescue`),
`xorriso`, `mtools`, `python3`; `qemu-system-x86` for testing.

```sh
sudo apt install build-essential gcc-multilib nasm grub-pc-bin grub-common xorriso mtools python3 qemu-system-x86
make              # -> build/kernel.elf, build/NovaOS.iso and a copy at ./NovaOS.iso
make WERROR=1     # the same with warnings as errors (the release builds warning-free)
make run          # boot the ISO in QEMU
make clean
```

`make BUILD=<dir>` puts all outputs in another directory,
`make CMDLINE="autorun=About"` bakes a kernel command line into the ISO
(options are listed in [docs/API.md](docs/API.md#17-kernel-command-line)).
Writing apps: [docs/API.md](docs/API.md), with `apps/hello.c` (About) and
`apps/widgets_demo.c` (Widget Gallery) as small working examples. Testing
and screenshots: [docs/TESTING.md](docs/TESTING.md);
`tools/collage.py` builds the all-apps picture above.

## Known limitations

NovaOS 2.9 is a **Developer Preview**. Things that are not there (yet):

* **Not yet tested on real VirtualBox or on real PCs.** All testing was done
  in QEMU (`qemu-system-i386`, PIIX chipset, IDE, PS/2, std VGA, AC'97 and
  SB16), which emulates the hardware VirtualBox presents; VirtualBox-specific
  behaviour (empty IDE channels, ACPI power button, VBE modes) was handled
  from its documentation, not observed on VirtualBox itself.
* **Basic networking** - only the Intel PRO/1000 (e1000) network card, IPv4
  only. The Web Browser has no JavaScript, so sites that need it (for
  example ChatGPT's chat) can't work, and its TLS (HTTPS) is hobby code:
  don't type real passwords or bank details.
* **No SATA/AHCI, NVMe, SCSI or USB** - hard disks must be on an IDE
  controller (PIO mode), and keyboard and mouse must be PS/2 (no USB tablet).
* **BIOS only** - no UEFI boot.
* **No VirtualBox Guest Additions** - no mouse integration (capture the
  mouse, Right Ctrl releases it), no shared folders, clipboard or automatic
  screen resizing.
* **Screen modes:** 1024x768 (or 800x600 safe mode), chosen at boot; no
  resolution change at run time.
* **Sound:** output only (no recording); Intel HD Audio is not supported;
  the PC speaker fallback plays one note at a time and is silent in
  VirtualBox.
* **One address space, ring 0, one CPU core:** apps are compiled into the
  kernel, there is no user mode and no memory protection between apps (only
  NULL-pointer accesses and writes to kernel code are caught); a crashing app
  stops the whole system (with a panic screen and stack trace). An app that
  stops responding (a callback that has not returned for 5 seconds) is
  suspended and the desktop goes on: *"App is not responding"* offers **Close
  the app** (the memory the app still holds is not freed) or **Wait**. You
  cannot install new apps without rebuilding.
* **Files:** the file system lives in memory. Live CD: files are lost at
  shut down. Installed system: the whole file system is saved as a snapshot
  to the drive (each of its two save slots takes half of the chosen drive
  size, at most 2 GB; what you can really fill is limited by memory); pulling
  the plug loses the last few seconds of changes. No
  access to other file systems (FAT, NTFS, ext) or CDs from inside NovaOS.
* **Pictures:** BMP only (Image Viewer also opens PPM); no PNG or JPEG.
* **Keyboard:** US layout only. One account per drive (no multi-user accounts);
  the start screen's password and the file protection guard against mistakes,
  not against someone with the disk image.

## Repository layout

```
NovaOS.iso   the release CD image
boot/        Multiboot header and entry point (NASM)
bootloader/  hard-disk boot loader: MBR stage 1 and stage 2 (NASM, embedded in the kernel)
kernel/      arch (GDT/IDT/PIC/PIT/FPU), core (boot info, heap, log, panic, power, ACPI),
             drivers (framebuffer/BGA, PCI, keyboard, mouse, RTC, ATA, sound:
             AC'97, Sound Blaster 16, PC speaker, software mixer),
             disk (installer back end, file persistence),
             gfx (surfaces, primitives, text, images, generated fonts),
             gui (window manager, compositor, desktop, bottom islands, Start
             menu, widgets, dialogs, theme, icon library), fs (VFS), lib (libc/libm),
             services (service manager, syslog, command registry, built-in
             daemons), shell (file associations, Open With / Properties / Run
             dialogs, pins, recent items)
apps/        the 34 applications (each file registers itself with REGISTER_APP)
include/     public API (nova.h and nova/*.h) and the libc headers
tools/       shot.py (headless QEMU test driver), wavcheck.py (notes in a WAV
             recording), collage.py (all-apps picture), genfont.py, gensyms.py
docs/        API.md (app developer guide), TESTING.md (test harness), screenshots/
```

What the installer writes to the disk (see `kernel/disk/diskfmt.h`): an MBR
with NovaOS's stage 1 boot code and a partition table, an install header,
the stage 2 loader (`bootloader/`: INT 13h extensions, A20, E820 memory map,
VBE linear framebuffer, protected mode, Multiboot-compatible hand-off with
`boot=disk` on the command line), the drive signature in sector 2047 (the
ISO's GRUB menu boots disks that have it by default), the kernel at 1 MiB, and
two save slots for your files - all within the drive size chosen on the NovaOS
Drive installation screen or in the Installer.

## Built with Claude

NovaOS was written by **Claude** (Anthropic's AI model, working as a coding
agent in Claude Code) at the request of, and directed by, the project's
owner: every commit in this repository was authored by Claude - about
56,000 lines of C and NASM, plus the build system, tools, tests and these
docs. Several Claude agents worked in parallel on different parts (apps,
drivers, installer, reviews) and checked their work by booting NovaOS in
QEMU and looking at screenshots. Nothing was copied from other operating
systems; the only third-party material is the two fonts below, rendered into
bitmaps at build time. As with any AI-written code, expect bugs - and
remember the limitations above: it has been tested in QEMU, not yet in
VirtualBox itself.

## Licence notes

The embedded fonts are rendered from Inter (SIL Open Font License 1.1) and
DejaVu Sans Mono (Bitstream Vera licence).
