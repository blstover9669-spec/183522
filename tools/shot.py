#!/usr/bin/env python3
"""
shot.py - boot NovaOS headless in QEMU, drive it with keys/mouse, take screenshots.

It never compiles anything: point it at an ISO or a kernel.elf you built.

Examples
  # boot the ISO, wait for the desktop, screenshot it
  tools/shot.py --iso build/NovaOS.iso --out shots ready shot:desktop

  # fast path: boot kernel.elf directly (QEMU -kernel, BGA framebuffer) and autorun an app
  tools/shot.py --kernel build/kernel.elf --cmdline "autorun=About" --out shots \\
      ready wait:1 shot:about

  # test the real GRUB/VBE path with a custom command line (builds a temp ISO)
  tools/shot.py --kernel build/kernel.elf --boot iso --cmdline "autorun=About" ready shot:x

  # interact: open the Start menu with the mouse, type to search, press Enter
  tools/shot.py --kernel build/kernel.elf --out shots ready click:28,744 wait:0.5 \\
      shot:start type:about key:ret wait:1 shot:launched

  # installer: install to a blank disk, then boot the installed system from it alone
  truncate -s 64M disk.img
  tools/shot.py --kernel build/kernel.elf --hda disk.img --cmdline "autorun=Installer:auto" \\
      ready "serial:INSTALL: done" shot:installed
  tools/shot.py --disk disk.img ready shot:from-disk

Steps (positional arguments, executed in order)
  ready                 wait until the kernel prints "NOVA: desktop ready" on serial
  serial:TEXT           wait until TEXT appears in the serial log
  wait:SECONDS          sleep (float)
  shot:NAME             save a screenshot to OUT/NAME.png
  key:NAMES             press keys, QEMU sendkey syntax (ret, esc, tab, f1, alt-f4, ctrl-alt-delete,
                        meta_l, up, down ...). Several presses separated by spaces in one quoted
                        step: "key:down down ret"
  type:TEXT             type text (letters, digits, punctuation, spaces; \\n = Enter)
  move:X,Y              move the mouse pointer to absolute screen coordinates X,Y
  click:X,Y[,BUTTON]    move and click (BUTTON = left|right|middle, default left)
  dclick:X,Y            move and double-click (left)
  drag:X1,Y1,X2,Y2      press at X1,Y1, move to X2,Y2, release
  scroll:N              mouse wheel N notches (positive = down)
  monitor:CMD           send a raw QEMU HMP monitor command

Absolute mouse positioning relies on the kernel option testmouse=1 (1:1 motion, no
acceleration), which this tool adds to the command line automatically in --kernel mode
and in --boot iso mode. With a prebuilt --iso, build it with CMDLINE="testmouse=1".
The pointer is first slammed into the top-left corner, then moved relatively.
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

READY_TEXT = "NOVA: desktop ready"

# characters -> QEMU sendkey names
KEYMAP = {
    " ": "spc", "\n": "ret", "\t": "tab", "-": "minus", "=": "equal", "[": "bracket_left",
    "]": "bracket_right", ";": "semicolon", "'": "apostrophe", "`": "grave_accent",
    "\\": "backslash", ",": "comma", ".": "dot", "/": "slash",
}
SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9",
    ")": "0", "_": "minus", "+": "equal", "{": "bracket_left", "}": "bracket_right",
    ":": "semicolon", '"': "apostrophe", "~": "grave_accent", "|": "backslash", "<": "comma",
    ">": "dot", "?": "slash",
}


def char_to_key(ch):
    if ch.isalpha() and ch.isascii():
        return ("shift-" + ch.lower()) if ch.isupper() else ch
    if ch.isdigit():
        return ch
    if ch in KEYMAP:
        return KEYMAP[ch]
    if ch in SHIFTED:
        return "shift-" + SHIFTED[ch]
    return None


class Monitor:
    def __init__(self, path, timeout=20):
        deadline = time.time() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise RuntimeError("cannot connect to QEMU monitor at " + path)
                time.sleep(0.1)
        self.sock.settimeout(30)
        self._read_prompt()

    def _read_prompt(self):
        buf = b""
        while not buf.endswith(b"(qemu) "):
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("QEMU monitor closed")
            buf += chunk
        return buf.decode("utf-8", "replace")

    def cmd(self, line):
        self.sock.sendall(line.encode() + b"\n")
        out = self._read_prompt()
        return out

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Session:
    def __init__(self, args):
        self.args = args
        self.out = os.path.abspath(args.out)
        os.makedirs(self.out, exist_ok=True)
        self.tmp = tempfile.mkdtemp(prefix="novashot-")
        self.mon_path = os.path.join(self.tmp, "mon.sock")
        self.serial = os.path.abspath(args.serial or os.path.join(self.out, "serial.log"))
        self.mouse = None      # (x, y) once known
        self.proc = None
        self.mon = None

    # -- QEMU lifecycle -------------------------------------------------------------
    def build_iso(self, kernel, cmdline):
        iso_dir = os.path.join(self.tmp, "iso")
        os.makedirs(os.path.join(iso_dir, "boot", "grub"))
        shutil.copy(kernel, os.path.join(iso_dir, "boot", "kernel.elf"))
        with open(os.path.join(iso_dir, "boot", "grub", "grub.cfg"), "w") as f:
            f.write("set timeout=0\nset default=0\ninsmod all_video\n"
                    "set gfxpayload=1024x768x32,1024x768x24,auto\n"
                    'menuentry "NovaOS" {\n  multiboot /boot/kernel.elf %s\n  boot\n}\n' % cmdline)
        iso = os.path.join(self.tmp, "test.iso")
        subprocess.run(["grub-mkrescue", "-o", iso, iso_dir], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return iso

    def start(self):
        a = self.args
        qemu = [a.qemu, "-m", str(a.mem), "-vga", a.vga, "-display", "none",
                "-serial", "file:" + self.serial,
                "-monitor", "unix:%s,server,nowait" % self.mon_path]
        if a.no_reboot:
            qemu.append("-no-reboot")
        cmdline = a.cmdline or ""
        if a.kernel and "testmouse" not in cmdline and not a.no_testmouse:
            cmdline = (cmdline + " testmouse=1").strip()
        if a.disk:
            # boot an installed system from its hard disk only (no CD, no -kernel)
            qemu += ["-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % a.disk, "-boot", "c"]
        elif a.iso:
            qemu += ["-cdrom", a.iso, "-boot", "d"]
        elif a.boot == "iso":
            qemu += ["-cdrom", self.build_iso(a.kernel, cmdline), "-boot", "d"]
        else:
            qemu += ["-kernel", a.kernel]
            if cmdline:
                qemu += ["-append", cmdline]
        if a.hda:
            qemu += ["-drive", "file=%s,format=raw,if=ide,index=0,media=disk" % a.hda]
        if a.cdrom2:
            qemu += ["-drive", "file=%s,media=cdrom,if=ide,index=2" % a.cdrom2]
        if os.path.exists("/dev/kvm") and a.kvm:
            qemu += ["-enable-kvm"]
        qemu += a.extra
        if os.path.exists(self.serial):
            os.remove(self.serial)
        if a.verbose:
            print("+", " ".join(qemu))
        self.proc = subprocess.Popen(qemu, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.mon = Monitor(self.mon_path)

    def stop(self):
        if self.mon:
            try:
                self.mon.cmd("quit")
            except Exception:
                pass
            self.mon.close()
        if self.proc:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def alive(self):
        return self.proc and self.proc.poll() is None

    # -- helpers ------------------------------------------------------------------------
    def serial_text(self):
        try:
            with open(self.serial, "r", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_serial(self, text, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if text in self.serial_text():
                return True
            if not self.alive():
                return False
            time.sleep(0.1)
        return False

    def screenshot(self, name):
        ppm = os.path.join(self.tmp, name + ".ppm")
        self.mon.cmd("screendump " + ppm)
        for _ in range(50):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                break
            time.sleep(0.05)
        png = os.path.join(self.out, name + ".png")
        if Image:
            Image.open(ppm).save(png)
        else:
            subprocess.run(["convert", ppm, png], check=True)
        os.remove(ppm)
        print("screenshot:", png)
        return png

    def key(self, spec):
        for k in spec.split():
            self.mon.cmd("sendkey " + k)
            time.sleep(self.args.key_delay)

    def type_text(self, text):
        text = text.replace("\\n", "\n")
        for ch in text:
            k = char_to_key(ch)
            if k is None:
                print("warning: cannot type %r" % ch, file=sys.stderr)
                continue
            self.mon.cmd("sendkey " + k)
            time.sleep(self.args.key_delay)

    def rel_move(self, dx, dy):
        # QEMU's PS/2 queue is small: send big moves in chunks
        step = 60
        while dx or dy:
            sx = max(-step, min(step, dx))
            sy = max(-step, min(step, dy))
            self.mon.cmd("mouse_move %d %d" % (sx, sy))
            dx -= sx
            dy -= sy
            time.sleep(0.012)

    def move(self, x, y):
        if self.mouse is None:
            self.rel_move(-4000, -4000)
            time.sleep(0.1)
            self.mouse = (0, 0)
        self.rel_move(x - self.mouse[0], y - self.mouse[1])
        self.mouse = (x, y)
        time.sleep(0.08)

    def button(self, mask):
        self.mon.cmd("mouse_button %d" % mask)
        time.sleep(0.06)

    def click(self, x, y, btn="left"):
        mask = {"left": 1, "right": 2, "middle": 4}[btn]
        self.move(x, y)
        self.button(mask)
        self.button(0)

    def run_step(self, step):
        kind, _, arg = step.partition(":")
        if kind == "ready":
            if not self.wait_serial(READY_TEXT, self.args.timeout):
                raise RuntimeError("desktop did not become ready within %ss" % self.args.timeout)
            time.sleep(0.3)
        elif kind == "serial":
            if not self.wait_serial(arg, self.args.timeout):
                raise RuntimeError("serial text %r not seen within %ss" % (arg, self.args.timeout))
        elif kind == "wait":
            time.sleep(float(arg))
        elif kind == "shot":
            self.screenshot(arg or "shot")
        elif kind == "key":
            self.key(arg)
        elif kind == "type":
            self.type_text(arg)
        elif kind == "move":
            x, y = [int(v) for v in arg.split(",")]
            self.move(x, y)
        elif kind == "click":
            parts = arg.split(",")
            self.click(int(parts[0]), int(parts[1]), parts[2] if len(parts) > 2 else "left")
        elif kind == "dclick":
            x, y = [int(v) for v in arg.split(",")]
            self.move(x, y)
            self.button(1); self.button(0)
            self.button(1); self.button(0)
        elif kind == "drag":
            x1, y1, x2, y2 = [int(v) for v in arg.split(",")]
            self.move(x1, y1)
            self.button(1)
            n = 8
            for i in range(1, n + 1):
                self.move(x1 + (x2 - x1) * i // n, y1 + (y2 - y1) * i // n)
            self.button(0)
        elif kind == "scroll":
            n = int(arg)
            for _ in range(abs(n)):
                self.mon.cmd("mouse_move 0 0 %d" % (1 if n > 0 else -1))
                time.sleep(0.05)
        elif kind == "monitor":
            print(self.mon.cmd(arg))
        else:
            raise RuntimeError("unknown step: " + step)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--iso", help="boot this ISO (as built, GRUB VBE path)")
    src.add_argument("--kernel", help="boot this kernel.elf (QEMU -kernel, or --boot iso)")
    src.add_argument("--disk", help="boot from this raw hard disk image only (installed NovaOS); "
                                    "the command line comes from the installation")
    ap.add_argument("--boot", choices=["kernel", "iso"], default="kernel",
                    help="with --kernel: boot directly (default) or via a temporary GRUB ISO")
    ap.add_argument("--cmdline", default="", help="kernel command line (--kernel mode)")
    ap.add_argument("--no-testmouse", action="store_true", help="do not add testmouse=1")
    ap.add_argument("--out", default="shots", help="output directory for PNGs and serial.log")
    ap.add_argument("--serial", help="serial log path (default OUT/serial.log)")
    ap.add_argument("--hda", help="raw disk image attached as IDE primary master")
    ap.add_argument("--cdrom2", help="extra CD image on the secondary IDE channel")
    ap.add_argument("--mem", type=int, default=256)
    ap.add_argument("--vga", default="std")
    ap.add_argument("--qemu", default="qemu-system-i386")
    ap.add_argument("--timeout", type=float, default=60, help="timeout for ready/serial steps")
    ap.add_argument("--key-delay", type=float, default=0.06)
    ap.add_argument("--no-reboot", action="store_true", help="exit instead of rebooting")
    ap.add_argument("--kvm", action="store_true", help="use KVM if available")
    ap.add_argument("--keep", action="store_true", help="leave QEMU running at the end until Ctrl+C")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[], help="extra QEMU args (must be last)")
    ap.add_argument("steps", nargs="*")
    args = ap.parse_args()

    s = Session(args)
    rc = 0
    try:
        s.start()
        for step in args.steps:
            if not s.alive():
                raise RuntimeError("QEMU exited early")
            if args.verbose:
                print("step:", step)
            s.run_step(step)
        if args.keep:
            print("QEMU running; Ctrl+C to stop")
            while s.alive():
                time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    except Exception as e:  # report and fail
        print("ERROR:", e, file=sys.stderr)
        rc = 1
    finally:
        s.stop()
    log = s.serial_text()
    print("serial log:", s.serial)
    for bad in ("KERNEL PANIC", "Heap corruption", "double panic"):
        if bad in log:
            print("ERROR: serial log contains %r" % bad, file=sys.stderr)
            rc = 2
    return rc


if __name__ == "__main__":
    sys.exit(main())
