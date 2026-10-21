#!/usr/bin/env python3
"""Disassemble main-ELF functions by name or address, with names from the
function map. Usage: disasm.py NAME_OR_0xADDR [...]  (needs capstone)"""
import struct, sys
from pathlib import Path
import capstone

ROOT = Path(__file__).resolve().parent.parent.parent
blob = (ROOT / "game" / "SLPM_666.75").read_bytes()
phoff = struct.unpack_from("<I", blob, 0x1C)[0]
segs = []
for i in range(struct.unpack_from("<H", blob, 0x2C)[0]):
    t, off, va, _, fs, _, _, _ = struct.unpack_from("<8I", blob, phoff + i * 32)
    if t == 1:
        segs.append((va, off, fs))
funcs = []
for line in (ROOT / "config" / "functions.recomp.csv").read_text().splitlines()[1:]:
    n, s, e, _ = line.split(",")
    funcs.append((int(s, 16), int(e, 16), n))
by_start = {s: n for s, e, n in funcs}

def read(va, n):
    for v, off, fs in segs:
        if v <= va < v + fs:
            return blob[off + va - v: off + va - v + n]
    raise SystemExit(f"0x{va:x} not in ELF")

md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 | capstone.CS_MODE_LITTLE_ENDIAN)
for arg in sys.argv[1:]:
    if arg.startswith("0x"):
        a = int(arg, 16)
        f = next(((s, e, n) for s, e, n in funcs if s <= a < e), (a, a + 0x80, "?"))
    else:
        f = next((x for x in funcs if x[2] == arg or arg in x[2]), None)
    s, e, n = f
    print(f"== {n} 0x{s:08X}-0x{e:08X}")
    code = read(s, e - s)
    for ins in md.disasm(code, s):
        extra = ""
        if ins.mnemonic in ("jal", "j"):
            t = int(ins.op_str, 16)
            extra = "  ; " + by_start.get(t, "")
        print(f"  {ins.address:08X}: {ins.mnemonic:8s} {ins.op_str}{extra}")
