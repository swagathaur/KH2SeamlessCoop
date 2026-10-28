#!/usr/bin/env python3
"""Make overlay function maps consistent with the overlays' own branches.

Within an overlay, every JAL target must be a function start and every J must
stay inside its function or land on a start. Targets in unmapped gaps become
functions; a call landing inside a function splits it; code after a `jr $ra`
that no branch connects to the rest is split off. Repeats to a fixpoint.

Usage: fix_overlay_maps.py <overlay> ...   (run from the repository root)
"""
import bisect
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from branchlib import ends_flow, extend_into_gaps, split_after_returns  # noqa: E402

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
    # Code pointers stored in the overlay's own data (vtables, callbacks): a
    # target in a gap counts if it starts a stack frame or directly follows a
    # function after zero padding (see make_function_map.py).
    starts = sorted(funcs)
    code_end = max(e for _, e in funcs.values())
    def owner(a):
        i = bisect.bisect_right(starts, a) - 1
        return starts[i] if i >= 0 and a < funcs[starts[i]][1] else None
    candidates = set()
    from_code = set()  # lui + addiu/ori constants: never switch-table case labels
    for va in range(BASE, limit - 3, 4):
        if not (va < code_end and owner(va) is not None):
            candidates.add(word(va))
    for s0 in list(starts):   # lui + addiu/ori address constants in code
        e0 = funcs[s0][1]
        for va in range(s0, e0, 4):
            w = word(va)
            if w >> 26 != 0x0F:
                continue
            rt, hi = (w >> 16) & 31, w & 0xFFFF
            for k in range(1, 9):
                if va + 4 * k >= e0:
                    break
                w2 = word(va + 4 * k)
                if w2 >> 26 in (0x09, 0x0D) and (w2 >> 21) & 31 == rt:
                    lo = w2 & 0xFFFF
                    if w2 >> 26 == 0x09 and lo & 0x8000:
                        lo -= 0x10000
                    candidates.add(((hi << 16) + lo) & 0xFFFFFFFF)
                    from_code.add(((hi << 16) + lo) & 0xFFFFFFFF)
                    break
    ptr_added = 0
    for t in sorted(candidates):
        if not (BASE <= t < code_end) or t & 3 or t in funcs:
            continue
        w = word(t)
        if not w:
            continue
        prologue = w >> 16 == 0x27BD and (w & 0xFFFF) >= 0x8000
        after_return = t - 8 >= BASE and (word(t - 8) == 0x03E00008 or
                                          (t in from_code and ends_flow(word(t - 8))))
        # (in a gap: after any jr, e.g. a switch's jr $t5 before its first case)
        jr_before = t - 8 >= BASE and (word(t - 8) & 0xFC1FFFFF) == 0x00000008
        own = owner(t)
        # A function that does a little before making its frame (stores a
        # vtable: lui/sw/addiu) was mapped from its frame: it starts here
        # (gumibattle 0x01F52B78, gumimenu 0x01F526B0).
        if own is None:
            i = bisect.bisect_right(starts, t)
            nxt = starts[i] if i < len(starts) else None
            if nxt is not None and nxt - t <= 16 and all(
                    word(a) >> 26 not in (1, 2, 3, 4, 5, 6, 7, 20, 21, 22, 23) and
                    not (word(a) >> 26 == 0 and (word(a) & 0x3F) in (8, 9))
                    for a in range(t, nxt, 4)):
                funcs[t] = [f"{name}_FUN_{t:08x}", funcs.pop(nxt)[1]]
                starts = sorted(funcs)
                ptr_added += 1
                continue
        if own is not None:
            if not (prologue or after_return):
                continue
            funcs[t] = [f"{name}_FUN_{t:08x}", funcs[own][1]]
            funcs[own][1] = t
        else:
            p = t - 4
            while p >= BASE and owner(p) is None and word(p) == 0:
                p -= 4
            # (or after a return, past any zero padding: a run of tiny leaf
            # functions in a gap, such as gumimenu's 0x01F6CF50 destructor)
            ret_before = p - 4 >= BASE and (word(p - 4) & 0xFC1FFFFF) == 0x00000008
            if not (prologue or after_return or jr_before or ret_before or owner(p) is not None):
                continue
            i = bisect.bisect_right(starts, t)
            nxt = starts[i] if i < len(starts) else limit
            end = nxt
            while end > t + 4 and word(end - 4) == 0:
                end -= 4
            funcs[t] = [f"{name}_FUN_{t:08x}", end]
        starts = sorted(funcs)
        ptr_added += 1
    total = ptr_added + extend_into_gaps(funcs, word, BASE, limit)
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
        added += split_after_returns(funcs, word, name + "_FUN_{:08x}")
        total += added
        if not added:
            break
    out = [lines[0]] + [f"{n},0x{a:08X},0x{e:08X},{e - a}" for a, (n, e) in sorted(funcs.items())]
    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"{name}: added/split {total}, now {len(funcs)} functions")
