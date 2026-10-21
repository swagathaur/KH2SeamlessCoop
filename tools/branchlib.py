"""Shared MIPS branch decoding for the function-map tools."""


def branch_target(va: int, w: int):
    """Target of a PC-relative branch (beq/bne/blez/bgtz + likely, REGIMM, bc1), else None."""
    op = w >> 26
    rel = False
    if 4 <= op <= 7 or 0x14 <= op <= 0x17:
        rel = True
    elif op == 1 and ((w >> 16) & 0x1F) in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13):
        rel = True
    elif op == 0x11 and ((w >> 21) & 0x1F) == 8:
        rel = True
    if not rel:
        return None
    off = w & 0xFFFF
    if off & 0x8000:
        off -= 0x10000
    return (va + 4 + (off << 2)) & 0xFFFFFFFF


def extend_into_gaps(funcs: dict, word, base: int, limit: int) -> int:
    """funcs: start -> [name, end]. Extend a function whose own branch lands in
    the unmapped gap right after it, up to the next function (minus padding)."""
    import bisect
    changed = 0
    for _ in range(10):
        starts = sorted(funcs)
        grew = 0
        for idx, s in enumerate(starts):
            end = funcs[s][1]
            nxt = starts[idx + 1] if idx + 1 < len(starts) else limit
            if end >= nxt:
                continue
            far = 0
            for va in range(s, end, 4):
                t = branch_target(va, word(va))
                if t is not None and end <= t < nxt:
                    far = max(far, t)
            if far:
                new_end = nxt
                while new_end > far + 4 and word(new_end - 4) == 0:
                    new_end -= 4
                funcs[s][1] = new_end
                grew += 1
        changed += grew
        if not grew:
            break
    return changed
