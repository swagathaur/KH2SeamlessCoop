#!/usr/bin/env python3
"""Extract everything the recomp needs from a KH2 Final Mix (JP) disc image.

Usage:  python tools/extract_game.py "Src/<your iso>.iso" [outdir=game]

Writes into `game/` (git-ignored, never commit it):
  SLPM_666.75            main EE executable (verified by SHA-256)
  SYSTEM.CNF, *.IRX, IOPRP300.IMG, PAD2/DS2O.IRX
  KH2.IDX, OVL.IDX       archive indexes
  overlays/<name>.x      EE code overlays from OVL.IMG, decompressed
                         (flat MIPS binaries linked at 0x01F18000)
"""
import hashlib, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from iso9660 import Iso
from kh2_compress import decompress, stored_size

ELF = "SLPM_666.75"
ELF_SHA256 = "d869ce4b8edd61cb978c4ff9ede595fe4d7d7b21cbb96a145275217120d8bc87"
LOOSE = ["SYSTEM.CNF", ELF, "IOPRP300.IMG", "SIO2MAN.IRX", "SIO2D.IRX", "DBCMAN.IRX",
         "MCMAN.IRX", "MCSERV.IRX", "LIBSD.IRX", "LIBSSL.IRX", "KH2.IDX", "OVL.IDX",
         "PAD2/DS2O.IRX"]
# Names referenced by the ELF; matched to OVL.IDX entries by filename hash.
OVERLAYS = ["ovl_title.x", "ovl_shop.x", "ovl_gumimenu.x", "ovl_gumibattle.x", "ovl_movie.x"]

def kh2_hash(name: str) -> int:
    """CRC-32 (poly 0x04C11DB7, MSB-first, init/xorout 0xFFFFFFFF) used by KH2 .IDX files."""
    c = 0xFFFFFFFF
    for b in name.encode():
        c ^= b << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
    return c ^ 0xFFFFFFFF

def read_idx(path):
    d = open(path, "rb").read()
    n = struct.unpack_from("<I", d, 0)[0]
    out = {}
    for i in range(n):
        h32, h16, block, sector, size = struct.unpack_from("<IHHII", d, 4 + i * 16)
        out[h32] = {"hash16": h16, "block": block, "sector": sector, "size": size}
    return out

def main():
    iso_path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "game"
    os.makedirs(os.path.join(out, "PAD2"), exist_ok=True)
    os.makedirs(os.path.join(out, "overlays"), exist_ok=True)
    iso = Iso(iso_path)

    for name in LOOSE:
        iso.extract(name, os.path.join(out, name))
    digest = hashlib.sha256(open(os.path.join(out, ELF), "rb").read()).hexdigest()
    if digest != ELF_SHA256:
        raise SystemExit(f"{ELF} hash mismatch: {digest}\nThis is not the build the decomp targets.")
    print(f"{ELF} OK (sha256 matches GovanifY/kh2)")

    idx = read_idx(os.path.join(out, "OVL.IDX"))
    base = iso.find("OVL.IMG")["lba"]
    for name in OVERLAYS:
        e = idx.get(kh2_hash(name))
        if e is None:
            raise SystemExit(f"overlay {name} not found in OVL.IDX")
        if not e["block"] & 0x4000:
            raise SystemExit(f"overlay {name} is not compressed; unexpected disc layout")
        raw = iso.read(base + e["sector"], stored_size(e["block"]))
        data = decompress(raw)
        if len(data) != e["size"]:
            raise SystemExit(f"overlay {name}: decompressed {len(data)} bytes, index says {e['size']}")
        with open(os.path.join(out, "overlays", name), "wb") as f:
            f.write(data)
        print(f"overlay {name:<18} {len(raw):>7} -> {len(data):>7} bytes")

if __name__ == "__main__":
    main()
