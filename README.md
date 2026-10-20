# NovaOS

NovaOS is a hobby operating system written from scratch in freestanding C and
NASM assembly - no Linux, no borrowed OS code. It boots from a CD image
straight into a modern-looking graphical desktop with windows, a taskbar, a
Start menu and apps, and runs in VirtualBox and QEMU. The **Installer** app
copies it onto a hard disk, which then boots NovaOS without the CD and keeps
your files across restarts.

* 32-bit x86 protected mode kernel, booted by GRUB (Multiboot 1) from the CD
  or by its own two-stage boot loader from an installed hard disk
* VBE linear framebuffer (1024x768x32 by default) with a Bochs/BGA fallback
* Double-buffered compositor with dirty rectangles, anti-aliased fonts
  (Inter + DejaVu Sans Mono), rounded windows and soft drop shadows
* Window manager: move, resize, maximize, minimize, snap to screen edges,
  Alt+Tab, z-order, focus; taskbar with pinned apps, live clock + calendar,
  Start menu with recent items and app / file / command search, desktop
  icons for apps and files, boot splash
* Background services run by a service manager (autostart, restart on
  failure, per-service logs, system log in /system/log/syslog):
  notifications (stacking toasts), screensaver (starfield + aurora), alarms,
  autosave, file indexer, system monitor - managed in the **Services** app
  or with `service` / `systemctl`
* Opening things: file associations with user-changeable defaults, Open With
  and Properties dialogs, desktop context menus (Open / Open With / Rename /
  Delete / Properties, New Folder / New Text File), Run dialog (Win+R) with
  autocomplete, taskbar pinning, a registry of shell commands any file can
  extend (`REGISTER_COMMAND`)
* Hand-drawn system icon library (folders, file types, devices, status
  icons) tuned for 16, 32 and 48 px - no image files, no emoji
* Widget toolkit: buttons, labels, text boxes, checkboxes, radio buttons,
  sliders, progress bars, list boxes, scroll bars, dropdowns, tabs, menu bars,
  context menus, message/input/file dialogs
* Light and dark themes, accent colours, procedural wallpapers
* PS/2 keyboard and mouse (with scroll wheel), PIT, CMOS clock, PC speaker,
  PCI enumeration, ACPI power-off, COM1 serial logging
* Heap allocator, in-memory hierarchical filesystem with (de)serialisation,
  libc/libm subset, kernel panic screen with symbolised stack traces
* ATA PIO hard disk driver (IDE primary/secondary, master/slave, LBA28/48),
  hard-disk installer wizard, and automatic saving of the file system to the
  installed disk
* Apps register themselves: drop a `.c` file into `apps/` and rebuild

## Building

Requirements (Debian/Ubuntu): `gcc` with 32-bit support (`gcc-multilib`),
`nasm`, `make`, `grub-pc-bin`, `grub-common` (grub-mkrescue), `xorriso`,
`mtools`, `python3`; `qemu-system-x86` for testing.

```sh
make            # -> build/kernel.elf and NovaOS.iso
make run        # boot the ISO in QEMU
```

`make BUILD=<dir>` puts all outputs in another directory,
`make CMDLINE="autorun=About"` bakes a kernel command line into the ISO.

## Running in VirtualBox

1. **New VM**: name `NovaOS`, type **Other**, version **Other/Unknown**
   (32-bit). Memory **256 MB** (64 MB minimum). A hard disk is only needed
   to install NovaOS (see [Installing to the hard disk](#installing-to-the-hard-disk)):
   create a **VDI of 64 MB or more** if you plan to.
2. **Settings -> System**: boot order with *Optical* first; leave **EFI
   disabled** (NovaOS boots via BIOS). Chipset PIIX3 is fine.
3. **Settings -> System -> Motherboard -> Pointing Device**: **PS/2 Mouse**
   (not USB Tablet).
4. **Settings -> Display**: graphics controller **VBoxVGA** (VMSVGA also
   works), video memory 32 MB or more.
5. **Settings -> Storage**: attach `NovaOS.iso` to the optical drive of the
   IDE controller.
6. Start the VM. The GRUB menu boots NovaOS after 2 seconds. Click inside the
   VM window to capture the mouse; the host key (Right Ctrl) releases it.

Or from the command line:

```sh
VBoxManage createvm --name NovaOS --ostype Other --register
VBoxManage modifyvm NovaOS --memory 256 --vram 32 --graphicscontroller vboxvga \
    --mouse ps2 --firmware bios --boot1 dvd --boot2 none
VBoxManage storagectl NovaOS --name IDE --add ide --controller PIIX4
VBoxManage storageattach NovaOS --storagectl IDE --port 1 --device 0 \
    --type dvddrive --medium "$PWD/NovaOS.iso"
VBoxManage startvm NovaOS
```

Troubleshooting: if the screen stays black, pick **NovaOS (safe graphics
800x600)** in the GRUB menu. If the pointer does not move, make sure the
pointing device is *PS/2 Mouse*. Shut down from the Start menu's power
button; VirtualBox then powers the VM off.

## Installing to the hard disk

Running from the CD keeps everything in memory: files you create are gone
after a restart. Installing puts NovaOS on the VM's hard disk, so it boots
without the ISO and saves your files.

1. **Add a disk** (VM powered off): *Settings -> Storage*, select the **IDE**
   controller (the one holding the CD drive), click *Adds hard disk* ->
   *Create*, **VDI**, **64 MB or more** (24 MB minimum). The disk must be on
   the IDE controller - NovaOS has no SATA/AHCI driver. Keep the VM settings
   from above: Other/Unknown, 256 MB, PS/2 mouse, VBoxVGA or VMSVGA.
2. **Boot the ISO** as usual (the ISO stays attached to the optical drive).
3. Open **Installer** (desktop icon, or Start menu -> System). Click
   *Next*, pick the disk (model and size are shown), *Next*, check the
   summary and click **Install**. All data on that disk is erased. Leave
   *Copy my current files to the new system* ticked to take the files you
   have made so far along. Installing takes a few seconds.
4. **Power off**: click *Shut down* on the last page (or Start menu ->
   power button -> Shut down).
5. **Remove the ISO**: *Settings -> Storage*, select the optical drive,
   disk icon -> *Remove Disk from Virtual Drive*. (While the VM runs you
   can also use *Devices -> Optical Drives -> Remove disk from virtual
   drive*.)
6. **Start the VM**. The BIOS boots the hard disk: you briefly see
   `NovaOS loader / Loading NovaOS .... ok`, then the desktop. Click into
   the VM window to capture the mouse; **Right Ctrl** (the host key)
   releases it.

If you leave the ISO attached, the CD boots first: choose **Boot from the
first hard disk (installed NovaOS)** in the GRUB menu, or remove the ISO.

**Your files on the installed system** are saved to the disk automatically
a few seconds after a change, at least every 60 seconds while there are
unsaved changes, and before every restart or shut down from the Start menu.
Pulling the plug (*Machine -> Power off*) loses at most the last few
seconds of changes; the previous save is never damaged (two save slots,
switched only after a complete write). Apps can force a save with
`persist_save()` (`<nova/disk.h>`). Running the Installer again on an
installed system can install to a second disk.

The same with `VBoxManage` (after the commands above):

```sh
VBoxManage createmedium disk --filename "$HOME/NovaOS-disk.vdi" --size 64 --format VDI
VBoxManage storageattach NovaOS --storagectl IDE --port 0 --device 0 \
    --type hdd --medium "$HOME/NovaOS-disk.vdi"
VBoxManage modifyvm NovaOS --boot1 dvd --boot2 disk
VBoxManage startvm NovaOS            # run the Installer, then shut down
VBoxManage storageattach NovaOS --storagectl IDE --port 1 --device 0 \
    --type dvddrive --medium emptydrive
VBoxManage startvm NovaOS            # boots from the hard disk
```

What the installer writes (see `kernel/disk/diskfmt.h`): the MBR with
NovaOS's stage 1 boot code and a partition table, an install header, the
stage 2 loader (`bootloader/`: INT 13h extensions, A20, E820 memory map,
VBE linear framebuffer 1024x768x32 / 800x600x32 / 1024x768x24, protected
mode, Multiboot-compatible hand-off with `boot=disk` on the command line),
the kernel image at 1 MiB on the disk, and the file storage area. If you
installed from the *safe graphics 800x600* GRUB entry, the installed system
keeps `res=800x600` and its loader picks that mode first.

In QEMU:

```sh
truncate -s 64M disk.img
qemu-system-i386 -m 256 -vga std -cdrom NovaOS.iso -hda disk.img -boot d   # install
qemu-system-i386 -m 256 -vga std -hda disk.img -boot c                     # installed system
```

## Using NovaOS

* Double-click desktop icons, or open the Start menu (bottom-left button,
  Windows key or Ctrl+Esc) and type to search.
* Drag windows by the title bar; drag to the top edge to maximize or to the
  left/right edge to snap. Alt+Tab switches windows, Alt+F4 closes one.
* Right-click the desktop for new folders / text files, wallpapers and dark
  mode; right-click a file icon to open it with another app, rename, delete
  it or see its properties. The clock opens a calendar; the speaker icon
  mutes the PC speaker.
* Win+R (or *Run...* in the Start menu) opens an app, file, folder or command
  by name. Right-click a Start menu entry or a taskbar button to pin the app
  to the taskbar.
* Background services (notifications, screensaver, alarms, ...) are listed in
  the **Services** app (System); `service list`, `notify`, `alarm +5 Tea`,
  `logs` and `assoc` work in the Terminal and the Run dialog.
* PrintScreen saves a screenshot to `/home/user/Pictures`.

## Repository layout

```
boot/        Multiboot header and entry point (NASM)
bootloader/  hard-disk boot loader: MBR stage 1 and stage 2 (NASM, embedded in the kernel)
kernel/      arch (GDT/IDT/PIC/PIT/FPU), core (boot info, heap, log, panic, power),
             drivers (framebuffer/BGA, PCI, keyboard, mouse, RTC, speaker, ATA),
             disk (installer back end, file persistence),
             gfx (surfaces, primitives, text, images, generated fonts),
             gui (window manager, compositor, desktop, taskbar, Start menu,
             widgets, dialogs, theme, icon library), fs (VFS), lib (libc/libm),
             services (service manager, syslog, command registry, built-in
             daemons), shell (file associations, shell_open, Open With /
             Properties / Run dialogs, pins, recent items)
apps/        applications (each file registers itself with REGISTER_APP)
include/     public API (nova.h and nova/*.h) and the libc headers
tools/       shot.py (headless QEMU test driver), genfont.py, gensyms.py
docs/        API.md (app developer guide), TESTING.md (test harness)
```

## Writing apps

See [docs/API.md](docs/API.md) for the full guide with a worked example, and
`apps/hello.c` (About) and `apps/widgets_demo.c` (Widget Gallery) for working
references. Testing and screenshots: [docs/TESTING.md](docs/TESTING.md).

## Licence notes

The embedded fonts are rendered from Inter (SIL Open Font License 1.1) and
DejaVu Sans Mono (Bitstream Vera licence).
