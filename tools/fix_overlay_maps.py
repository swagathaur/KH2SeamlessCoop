#!/usr/bin/env python3
"""Make overlay function maps consistent with the overlays' own branches.

Within an overlay, every JAL target must be a function start and every J must
stay inside its function or land on a start. Targets in unmapped gaps become
functions; a call landing inside a function splits it. Repeats to a fixpoint.

Usage: fix_overlay_maps.py <overlay> ...   (run from the repository root)
"""
import bisect
import struct
import sys
from pathlib import Path

BASE, ARENA_END = 0x01F18000, 0x01FC0000

for name in sys.argv[1:]:
    image = Path(f"game/overlays/ovl_{name}.x").read_bytes()
    path = Path(f"config/functions_{name}.csv")
    lines = path.read_text().splitlines()
    funcs = {}
    for l in lines[1:]:
        n, a, b, _ = l.split(",")
        funcs[int(a, 16)] = [n, int(b, 16)]
    limit = BASE + len(image)
    word = lambda va: struct.unpack_from("<I", image, va - BASE)[0]
    total = 0
    for _ in range(20):
        starts = sorted(funcs)
        added = 0
        for s in list(starts):
            for va in range(s, funcs[s][1], 4):
                w = word(va)
                op = w >> 26
                if op not in (2, 3):
                    continue
                t = (va & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if not (BASE <= t < limit) or t in funcs:
                    continue
                i = bisect.bisect_right(starts, t) - 1
                owner = starts[i] if i >= 0 and t < funcs[starts[i]][1] else None
                if owner is None:
                    nxt = starts[i + 1] if i + 1 < len(starts) else limit
                    end = nxt
                    while end > t + 4 and word(end - 4) == 0:
                        end -= 4
                    funcs[t] = [f"{name}_FUN_{t:08x}", end]
                elif op == 3 or owner != s:
                    funcs[t] = [f"{name}_FUN_{t:08x}", funcs[owner][1]]
                    funcs[owner][1] = t
                else:
                    continue
                starts = sorted(funcs)
                added += 1
        total += added
        if not added:
            break
    out = [lines[0]] + [f"{n},0x{a:08X},0x{e:08X},{e - a}" for a, (n, e) in sorted(funcs.items())]
    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{name}: added/split {total}, now {len(funcs)} functions")
