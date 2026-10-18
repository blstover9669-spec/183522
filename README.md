# NovaOS

NovaOS is a hobby operating system written from scratch in freestanding C and
NASM assembly - no Linux, no borrowed OS code. It boots from a CD image
straight into a modern-looking graphical desktop with windows, a taskbar, a
Start menu and apps, and runs in VirtualBox and QEMU.

* 32-bit x86 protected mode kernel, booted by GRUB (Multiboot 1)
* VBE linear framebuffer (1024x768x32 by default) with a Bochs/BGA fallback
* Double-buffered compositor with dirty rectangles, anti-aliased fonts
  (Inter + DejaVu Sans Mono), rounded windows and soft drop shadows
* Window manager: move, resize, maximize, minimize, snap to screen edges,
  Alt+Tab, z-order, focus; taskbar with live clock + calendar, searchable
  Start menu, desktop icons, toast notifications, boot splash
* Widget toolkit: buttons, labels, text boxes, checkboxes, radio buttons,
  sliders, progress bars, list boxes, scroll bars, dropdowns, tabs, menu bars,
  context menus, message/input/file dialogs
* Light and dark themes, accent colours, procedural wallpapers
* PS/2 keyboard and mouse (with scroll wheel), PIT, CMOS clock, PC speaker,
  PCI enumeration, ACPI power-off, COM1 serial logging
* Heap allocator, in-memory hierarchical filesystem with (de)serialisation,
  libc/libm subset, kernel panic screen with symbolised stack traces
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
   (32-bit). Memory **256 MB** (64 MB minimum). No hard disk is needed.
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

## Using NovaOS

* Double-click desktop icons, or open the Start menu (bottom-left button,
  Windows key or Ctrl+Esc) and type to search.
* Drag windows by the title bar; drag to the top edge to maximize or to the
  left/right edge to snap. Alt+Tab switches windows, Alt+F4 closes one.
* Right-click the desktop for wallpapers and dark mode. The clock opens a
  calendar; the speaker icon mutes the PC speaker.
* PrintScreen saves a screenshot to `/home/user/Pictures`.

## Repository layout

```
boot/        Multiboot header and entry point (NASM)
kernel/      arch (GDT/IDT/PIC/PIT/FPU), core (boot info, heap, log, panic, power),
             drivers (framebuffer/BGA, PCI, keyboard, mouse, RTC, speaker),
             gfx (surfaces, primitives, text, images, generated fonts),
             gui (window manager, compositor, desktop, taskbar, Start menu,
             widgets, dialogs, theme), fs (VFS), lib (libc/libm)
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
