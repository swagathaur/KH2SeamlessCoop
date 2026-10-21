#!/usr/bin/env python3
"""Extract named files from KH2.IMG on the disc (by KH2.IDX hash).

Usage:  python tools/extract_file.py "Src/<your iso>.iso" [--world tt] NAME [NAME...]
Writes game/files/<name> (git-ignored game data). Compressed entries are
decompressed. Names are as the game asks for them, e.g. "00objentry.bin",
"03system.bin", "obj/P_EX110.mdlx". World files (rooms' "ard/tt02.ard" and
the like) are listed in that world's own index, 000<world>.idx inside
KH2.IMG: pass --world with the two-letter world code to look there (the
names are tried in KH2.IDX too).
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso9660 import Iso
from kh2_compress import decompress, stored_size
from extract_game import kh2_hash, read_idx


def parse_idx(d):
    n = struct.unpack_from("<I", d, 0)[0]
    out = {}
    for i in range(n):
        h32, h16, block, sector, size = struct.unpack_from("<IHHII", d, 4 + i * 16)
        out[h32] = {"hash16": h16, "block": block, "sector": sector, "size": size}
    return out


def read_entry(iso, base, e):
    if e["block"] & 0x4000:
        data = decompress(iso.read(base + e["sector"], stored_size(e["block"])))
    else:
        data = iso.read(base + e["sector"], e["size"])
    return data[: e["size"]]


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    iso = Iso(args.pop(0))
    idxs = [read_idx(os.path.join("game", "KH2.IDX"))]
    base = iso.find("KH2.IMG")["lba"]
    if args and args[0] == "--world":
        world = args[1]
        args = args[2:]
        sub = idxs[0].get(kh2_hash(f"000{world}.idx"))
        if sub is None:
            raise SystemExit(f"000{world}.idx: not in KH2.IDX")
        idxs.insert(0, parse_idx(read_entry(iso, base, sub)))
    for name in args:
        e = next((i[kh2_hash(name)] for i in idxs if kh2_hash(name) in i), None)
        if e is None:
            print(f"{name}: not found")
            continue
        data = read_entry(iso, base, e)
        path = os.path.join("game", "files", name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes -> {path}")


if __name__ == "__main__":
    main()
