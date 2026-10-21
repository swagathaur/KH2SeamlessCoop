#!/usr/bin/env python3
"""Extract named files from KH2.IMG on the disc (by KH2.IDX hash).

Usage:  python tools/extract_file.py "Src/<your iso>.iso" 00objentry.bin [more names...]
Writes game/files/<name> (git-ignored game data). Compressed entries are
decompressed. Names are as the game asks for them, e.g. "00objentry.bin",
"03system.bin", "obj/P_EX110.mdlx".
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso9660 import Iso
from kh2_compress import decompress, stored_size
from extract_game import kh2_hash, read_idx


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    iso = Iso(sys.argv[1])
    idx = read_idx(os.path.join("game", "KH2.IDX"))
    base = iso.find("KH2.IMG")["lba"]
    for name in sys.argv[2:]:
        e = idx.get(kh2_hash(name))
        if e is None:
            print(f"{name}: not in KH2.IDX")
            continue
        if e["block"] & 0x4000:
            data = decompress(iso.read(base + e["sector"], stored_size(e["block"])))
        else:
            data = iso.read(base + e["sector"], e["size"])
        data = data[: e["size"]]
        path = os.path.join("game", "files", name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes -> {path}")


if __name__ == "__main__":
    main()
