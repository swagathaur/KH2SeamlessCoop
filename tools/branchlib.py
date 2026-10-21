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


def ends_flow(w: int) -> bool:
    """An unconditional transfer: execution never falls past its delay slot.
    jr $ra, j, or b (beq $zero, $zero)."""
    return w == 0x03E00008 or w >> 26 == 2 or w >> 16 == 0x1000


def jump_target(va: int, w: int):
    """Target of J/JAL, else None."""
    if w >> 26 in (2, 3):
        return (va & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
    return None


def split_after_returns(funcs: dict, word, name_fmt="FUN_{:08x}") -> int:
    """funcs: start -> [name, end]. Split off code that follows an unconditional
    transfer (`jr $ra`, `j`, `b`) and its delay slot when no branch connects the
    two parts.

    Ghidra sometimes merges adjacent leaf functions reached only through
    pointers (qsort/bsearch comparators at 0x003216A8/0x003216B8, a callback
    after `jr $ra; b ...` at 0x001E99F0). If nothing before the split point
    branches past it and nothing after it branches back, the code after it is a
    separate entry. Functions with a register jump
    other than `jr $ra` (switch tables) are left alone: a table can point past
    an early return.
    """
    JR_RA = 0x03E00008
    added = 0
    work = sorted(funcs)
    while work:
        s = work.pop()
        name, e = funcs[s]
        words = [(va, word(va)) for va in range(s, e, 4)]
        if any((w & 0xFC1FFFFF) == 0x00000008 and w != JR_RA for _, w in words):
            continue  # jr $rs with rs != ra: a jump table
        edges = []
        for va, w in words:
            t = branch_target(va, w)
            if t is None:
                t = jump_target(va, w)
            if t is not None and s <= t < e:
                edges.append((va, t))
        for va, w in words:
            if not ends_flow(w) or va + 8 >= e:
                continue
            c = va + 8
            while c < e and word(c) == 0:
                c += 4
            if c >= e or c in funcs:
                continue
            if any((src < c) != (dst < c) for src, dst in edges):
                continue  # a branch crosses the would-be boundary
            funcs[s][1] = va + 8
            funcs[c] = [name_fmt.format(c), e]
            work.append(c)
            added += 1
            break
    return added
