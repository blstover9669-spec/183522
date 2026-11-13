#!/usr/bin/env python3
"""sysdamage.py - damage one byte of a file inside an installed NovaOS disk image.

Test tool for the system file manifest (kernel/fs/sysfiles.c): it flips one
byte of FILE in the newest save of the drive's files and fixes the save's
checksums (the VFS image checksum, the slot's data CRC32 and header CRC32), so
the save itself is still valid and only that file is damaged - like a disk
sector that went bad. The next start of the drive must stop with the kernel
panic "A system file is damaged: FILE".

    tools/sysdamage.py disk.img /system/ui/fonts/ui.font [--offset N]
    tools/sysdamage.py disk.img --list          # the files in the newest save

Layout (kernel/disk/diskfmt.h, kernel/fs/vfs.c): install header at LBA 1,
two save slots at persist_lba, each = header sector + vfs_serialize() image.
"""
import argparse
import struct
import sys
import zlib

SECTOR = 512


def read_sector(f, lba, n=1):
    f.seek(lba * SECTOR)
    return f.read(n * SECTOR)


def slot_header(f, lba, disk_id):
    h = read_sector(f, lba)
    m0, m1, ver, did, seq, length, dcrc, _t = struct.unpack_from("<8I", h, 0)
    if (m0, m1) != (0x41564F4E, 0x53524550) or ver != 1 or did != disk_id:
        return None
    if zlib.crc32(h[:508]) & 0xFFFFFFFF != struct.unpack_from("<I", h, 508)[0]:
        return None
    return {"lba": lba, "seq": seq, "length": length, "crc": dcrc, "raw": bytearray(h)}


def parse_nodes(img):
    count = struct.unpack_from("<I", img, 8)[0]
    pos, nodes = 16, []
    for i in range(count):
        parent, is_dir, nl = struct.unpack_from("<IBB", img, pos)
        name = img[pos + 6:pos + 6 + nl].decode("utf-8", "replace")
        size = struct.unpack_from("<I", img, pos + 6 + nl + 8)[0]
        data = pos + 6 + nl + 12
        path = "" if i == 0 else (nodes[parent]["path"].rstrip("/") + "/" + name)
        nodes.append({"path": path or "/", "dir": is_dir, "size": size, "data": data})
        pos = data + size
    return nodes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disk")
    ap.add_argument("file", nargs="?")
    ap.add_argument("--offset", type=int, default=None, help="byte of the file to flip (default: the middle)")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    with open(a.disk, "r+b") as f:
        hdr = read_sector(f, 1)
        if struct.unpack_from("<2I", hdr, 0) != (0x41564F4E, 0x4B534944):
            sys.exit("not an installed NovaOS drive (no install header at LBA 1)")
        plba, psec = struct.unpack_from("<2I", hdr, 44)
        disk_id = struct.unpack_from("<I", hdr, 56)[0]
        slots = [s for s in (slot_header(f, plba, disk_id), slot_header(f, plba + psec, disk_id)) if s]
        if not slots:
            sys.exit("the drive has no saved files yet (its first start has not run)")
        s = max(slots, key=lambda x: x["seq"])
        img = bytearray(read_sector(f, s["lba"] + 1, (s["length"] + SECTOR - 1) // SECTOR)[:s["length"]])
        nodes = parse_nodes(img)
        if a.list or not a.file:
            for n in nodes:
                print(("%10s  " % ("<dir>" if n["dir"] else n["size"])) + n["path"])
            return
        node = next((n for n in nodes if n["path"] == a.file and not n["dir"]), None)
        if not node or not node["size"]:
            sys.exit("%s: no such (non-empty) file in the save" % a.file)
        off = node["size"] // 2 if a.offset is None else a.offset
        if not 0 <= off < node["size"]:
            sys.exit("offset out of range (the file has %d bytes)" % node["size"])
        img[node["data"] + off] ^= 0x5A
        chk = 0
        for b in img[16:]:
            chk = (chk * 31 + b) & 0xFFFFFFFF
        struct.pack_into("<I", img, 12, chk)
        raw = s["raw"]
        struct.pack_into("<I", raw, 24, zlib.crc32(bytes(img)) & 0xFFFFFFFF)
        struct.pack_into("<I", raw, 508, zlib.crc32(bytes(raw[:508])) & 0xFFFFFFFF)
        f.seek((s["lba"] + 1) * SECTOR)
        f.write(img)
        f.seek(s["lba"] * SECTOR)
        f.write(raw)
        print("damaged byte %d of %s (save #%d, slot at lba %d)" % (off, a.file, s["seq"], s["lba"]))


if __name__ == "__main__":
    main()
