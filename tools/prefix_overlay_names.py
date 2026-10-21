#!/usr/bin/env python3
"""Prefix every function name in an overlay map with the overlay's name.

All overlays occupy the same addresses, so unprefixed names (FUN_01f18120)
collide when the five translated overlays are linked into one executable.

Usage: prefix_overlay_names.py <overlay> <csv>
"""
import sys

name, path = sys.argv[1], sys.argv[2]
lines = open(path, encoding="utf-8").read().splitlines()
out = [lines[0]]
for line in lines[1:]:
    if not line:
        continue
    fn, rest = line.split(",", 1)
    if not fn.startswith(name + "_"):
        fn = f"{name}_{fn}"
    out.append(f"{fn},{rest}")
open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
