#!/usr/bin/env python3
"""Find lui/(addiu|lw|sw|lbu|sb|lh|sh) pairs referencing an absolute address.
Usage: refs.py 0xADDR [window]"""
import struct, sys, bisect
from pathlib import Path
ROOT = Path(__file__).resolve().parent.parent.parent
blob = (ROOT / "game" / "SLPM_666.75").read_bytes()
phoff = struct.unpack_from("<I", blob, 0x1C)[0]
segs = []
for i in range(struct.unpack_from("<H", blob, 0x2C)[0]):
    t, off, va, _, fs, _, _, _ = struct.unpack_from("<8I", blob, phoff + i * 32)
    if t == 1:
        segs.append((va, off, fs))
funcs = sorted((int(s, 16), int(e, 16), n) for n, s, e, _ in
               (l.split(",") for l in (ROOT / "config" / "functions.recomp.csv").read_text().splitlines()[1:]))
starts = [f[0] for f in funcs]
def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return funcs[i][2] if i >= 0 and funcs[i][0] <= a < funcs[i][1] else "?"
target = int(sys.argv[1], 16)
window = int(sys.argv[2]) if len(sys.argv) > 2 else 0
ops = {0x09: "addiu", 0x23: "lw", 0x2B: "sw", 0x24: "lbu", 0x28: "sb", 0x25: "lhu", 0x29: "sh", 0x20: "lb", 0x21: "lh", 0x31: "lwc1", 0x39: "swc1", 0x37: "ld", 0x3F: "sd", 0x1E: "lq", 0x1F: "sq"}
for va, off, fs in segs:
    for i in range(0, fs - 4, 4):
        w = struct.unpack_from("<I", blob, off + i)[0]
        if w >> 26 != 0xF:
            continue
        rt = (w >> 16) & 31
        hi = (w & 0xFFFF) << 16
        for k in range(1, 10):
            w2 = struct.unpack_from("<I", blob, off + i + 4 * k)[0]
            op = w2 >> 26
            if op in ops and (w2 >> 21) & 31 == rt:
                imm = w2 & 0xFFFF
                addr = (hi + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
                if target <= addr <= target + window:
                    print(f"{va+i+4*k:08X} {ops[op]:6s} 0x{addr:08X} in {owner(va+i)}")
                break
