#!/usr/bin/env python3
"""List jal/j call sites of main-ELF functions. Usage: callers.py 0xADDR [...]"""
import struct, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
blob = (ROOT / "game" / "SLPM_666.75").read_bytes()
phoff = struct.unpack_from("<I", blob, 0x1C)[0]
segs = []
for i in range(struct.unpack_from("<H", blob, 0x2C)[0]):
    t, off, va, _, fs, flags, _, _ = struct.unpack_from("<8I", blob, phoff + i * 32)
    if t == 1:
        segs.append((va, off, fs))
funcs = []
for line in (ROOT / "config" / "functions.recomp.csv").read_text().splitlines()[1:]:
    n, s, e, _ = line.split(",")
    funcs.append((int(s, 16), int(e, 16), n))
funcs.sort()
import bisect
starts = [f[0] for f in funcs]
def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return funcs[i][2] if i >= 0 and funcs[i][0] <= a < funcs[i][1] else "?"
targets = {int(x, 16) for x in sys.argv[1:]}
for va, off, fs in segs:
    for i in range(0, fs - 3, 4):
        w = struct.unpack_from("<I", blob, off + i)[0]
        op = w >> 26
        if op in (2, 3):
            t = ((va + i + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if t in targets:
                print(f"{va+i:08X} {'jal' if op == 3 else 'j  '} {t:08X}  in {owner(va+i)}")
