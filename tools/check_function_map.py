#!/usr/bin/env python3
"""Static check of a function map against the ELF's direct jumps and calls.

Every JAL must target a function start; every J must target a function start
or stay within its own function. Violations fail at runtime as
"[guest-branch:missing-target]".

Usage: check_function_map.py config/functions.csv game/SLPM_666.75
"""
import bisect
import csv
import struct
import sys

TEXT_VA, TEXT_OFF, TEXT_SIZE = 0x00100000, 0x1000, 0x27A4B4

rows = list(csv.DictReader(open(sys.argv[1], encoding="utf-8")))
elf = open(sys.argv[2], "rb").read()
text = elf[TEXT_OFF:TEXT_OFF + TEXT_SIZE]
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
        w = struct.unpack_from("<I", text, va - TEXT_VA)[0]
        op = w >> 26
        if op not in (2, 3):
            continue
        t = (va & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        if not (TEXT_VA <= t < TEXT_VA + TEXT_SIZE) or t in startset:
            continue
        if op == 2 and owner(t) == s:
            continue
        bad.append((va, "jal" if op == 3 else "j", t, names[s], names.get(owner(t), "-") if owner(t) else "(gap)"))
print(f"{len(bad)} problem branches")
for va, k, t, src, dst in bad[:40]:
    print(f"  {va:08x} {k:3} -> {t:08x}  in {src}  target in {dst}")
