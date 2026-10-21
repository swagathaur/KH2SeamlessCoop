#!/usr/bin/env python3
"""Clamp each function's end to the next function's start in a function map CSV.

Usage: clamp_function_maps.py config/functions_*.csv
"""
import sys

for path in sys.argv[1:]:
    lines = open(path, encoding="utf-8").read().splitlines()
    rows = [l.split(",") for l in lines[1:] if l]
    rows.sort(key=lambda r: int(r[1], 16))
    fixed = 0
    for i, r in enumerate(rows):
        start, end = int(r[1], 16), int(r[2], 16)
        if i + 1 < len(rows):
            nxt = int(rows[i + 1][1], 16)
            if end > nxt:
                end, fixed = nxt, fixed + 1
        r[2], r[3] = f"0x{end:08X}", str(end - start)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(lines[0] + "\n" + "".join(",".join(r) + "\n" for r in rows))
    print(f"{path}: clamped {fixed} of {len(rows)}")
