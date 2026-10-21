#!/usr/bin/env python3
"""Find the player's position in guest RAM from harness --ram-dump snapshots.

Usage: find_position.py ram_A.bin ram_B.bin [ram_C.bin ...]
The first snapshot should be taken standing still and the later ones while
moving (tools/scripts/walk-test.txt). Prints 16-byte-aligned float vectors
whose xyz moved a plausible distance between every pair of later snapshots,
grouped by how many copies of the same value exist (the actor's own transform
usually has a few mirrors: render matrix, collision, shadow).
"""
import sys
import numpy as np

dumps = [np.fromfile(p, dtype="<f4").reshape(-1, 4) for p in sys.argv[1:]]
n = len(dumps)
stack = np.stack(dumps)  # (n, rows, 4)
xyz = stack[:, :, :3]
w = stack[:, :, 3]
finite = np.isfinite(stack).all(axis=(0, 2))
sane = finite & (np.abs(xyz) < 1e5).all(axis=(0, 2)) & (np.abs(xyz) > 1e-3).any(axis=2).all(axis=0)
step = np.linalg.norm(np.diff(xyz, axis=0), axis=2)  # (n-1, rows)
moving = (step[1:] > 1.0).all(axis=0) & (step[1:] < 5000).all(axis=0) if n > 2 else step[0] > 1.0
cand = np.where(sane & moving)[0]
print(f"{len(cand)} candidate vectors")
rows = []
for r in cand:
    rows.append((r * 16, xyz[:, r], w[:, r], step[:, r]))
# Rank: w == 1 first (homogeneous position), then larger total movement.
rows.sort(key=lambda t: (abs(t[2][0] - 1.0) > 1e-6, -t[3].sum()))
for addr, p, wv, st in rows[:80]:
    path = "  ".join(f"({x:9.1f},{y:9.1f},{z:9.1f})" for x, y, z in p)
    print(f"0x{addr:08X} w={wv[0]:g} steps={','.join(f'{s:.0f}' for s in st)}  {path}")
