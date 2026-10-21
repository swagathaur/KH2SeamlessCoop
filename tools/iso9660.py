#!/usr/bin/env python3
"""Minimal dependency-free ISO9660 reader for PS2 discs.

Usage:
  iso9660.py ls  <iso> [path]          list a directory (default /)
  iso9660.py tree <iso>                 list every file with LBA and size
  iso9660.py get <iso> <path> <out>     extract one file
"""
import struct, sys

SECTOR = 2048

class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")
        pvd = self.read(16, SECTOR)
        if pvd[1:6] != b"CD001":
            raise SystemExit("not an ISO9660 image")
        self.root = self._record(pvd[156:190])

    def read(self, lba, size):
        self.f.seek(lba * SECTOR)
        return self.f.read(size)

    @staticmethod
    def _record(b):
        lba = struct.unpack_from("<I", b, 2)[0]
        size = struct.unpack_from("<I", b, 10)[0]
        flags = b[25]
        nlen = b[32]
        name = b[33:33 + nlen].decode("ascii", "replace").split(";")[0]
        return {"lba": lba, "size": size, "dir": bool(flags & 2), "name": name}

    def listdir(self, rec):
        data = self.read(rec["lba"], rec["size"])
        out, i = [], 0
        while i < len(data):
            ln = data[i]
            if ln == 0:  # records never span sectors; skip to next sector
                i = (i // SECTOR + 1) * SECTOR
                continue
            r = self._record(data[i:i + ln])
            if r["name"] not in ("\x00", "\x01"):
                out.append(r)
            i += ln
        return out

    def find(self, path):
        rec = self.root
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            match = [r for r in self.listdir(rec) if r["name"].upper() == part.upper()]
            if not match:
                raise SystemExit(f"not found: {path}")
            rec = match[0]
        return rec

    def walk(self, rec=None, prefix=""):
        rec = rec or self.root
        for r in self.listdir(rec):
            p = f"{prefix}/{r['name']}"
            if r["dir"]:
                yield from self.walk(r, p)
            else:
                yield p, r

    def extract(self, path, out):
        r = self.find(path)
        self.f.seek(r["lba"] * SECTOR)
        left = r["size"]
        with open(out, "wb") as o:
            while left:
                chunk = self.f.read(min(left, 1 << 20))
                o.write(chunk)
                left -= len(chunk)

if __name__ == "__main__":
    cmd, iso = sys.argv[1], Iso(sys.argv[2])
    if cmd == "ls":
        for r in iso.listdir(iso.find(sys.argv[3] if len(sys.argv) > 3 else "/")):
            print(f"{'d' if r['dir'] else '-'} {r['lba']:>9} {r['size']:>12} {r['name']}")
    elif cmd == "tree":
        for p, r in iso.walk():
            print(f"{r['lba']:>9} {r['size']:>12} {p}")
    elif cmd == "get":
        iso.extract(sys.argv[3], sys.argv[4])
