#!/usr/bin/env python3
"""Static check of a function map against direct jumps, calls and branches.

Fails at runtime as "[guest-branch:missing-target]":
  - a JAL to an address that is not a function start
  - a J or conditional branch to an address outside every function (a gap)
Branches into the middle of another function are fine: ps2_recomp registers
internal entry points for them.

Usage: check_function_map.py config/functions.csv game/SLPM_666.75
       check_function_map.py --overlay <name>      (run from the repository root)
"""
import bisect
import csv
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from branchlib import branch_target  # noqa: E402

if sys.argv[1] == "--overlay":
    name = sys.argv[2]
    csv_path, image, base = f"config/functions_{name}.csv", Path(f"game/overlays/ovl_{name}.x").read_bytes(), 0x01F18000
else:
    csv_path, base = sys.argv[1], 0x00100000
    image = Path(sys.argv[2]).read_bytes()[0x1000:0x1000 + 0x27A4B4]
limit = base + len(image)

rows = list(csv.DictReader(open(csv_path, encoding="utf-8")))
starts = sorted(int(r["Start"], 16) for r in rows)
ends = {int(r["Start"], 16): int(r["End"], 16) for r in rows}
names = {int(r["Start"], 16): r["Name"] for r in rows}
startset = set(starts)


def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return starts[i] if i >= 0 and a < ends[starts[i]] else None


bad = []
for s in starts:
    for va in range(s, ends[s], 4):
        w = struct.unpack_from("<I", image, va - base)[0]
        t = branch_target(va, w)
        kind = "br"
        if t is None:
            if w >> 26 not in (2, 3):
                continue
            kind = "jal" if w >> 26 == 3 else "j"
            t = (va & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        if not (base <= t < limit) or t in startset:
            continue
        if owner(t) is None or kind == "jal":
            bad.append((va, kind, t, names[s], names[owner(t)] if owner(t) else "(gap)"))
print(f"{len(bad)} problem branches")
for va, k, t, src, dst in bad[:40]:
    print(f"  {va:08x} {k:3} -> {t:08x}  in {src}  target in {dst}")
