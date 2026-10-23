#!/usr/bin/env python3
"""Disassemble PS2 VU microcode (upper/lower instruction pairs).

    vu_disasm.py <file> [--offset N] [--pairs N] [--base PC]

<file> holds raw 64-bit pairs (lower word first, then upper). For a VIF MPG
packet in a RAM dump, point --offset at the first pair after the MPG code.
Covers the instructions the VU manual lists; unknown encodings print as .word.
"""
import argparse
import struct

DEST = lambda d: ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if d & b)
BC = 'xyzw'


def upper(w):
    d = DEST((w >> 21) & 15)
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    flags = ''.join(f for f, b in (('[I]', 31), ('[E]', 30), ('[M]', 29), ('[D]', 28), ('[T]', 27)) if w >> b & 1)
    op = w & 0x3F
    bc = BC[w & 3]
    names = {0x00: 'add', 0x04: 'sub', 0x08: 'madd', 0x0C: 'msub', 0x10: 'max', 0x14: 'mini', 0x18: 'mul'}
    if op < 0x20 and (op & ~3) in names:
        return f"{names[op & ~3]}{bc}.{d} vf{fd}, vf{fs}, vf{ft}{bc}{flags}"
    simple = {0x1C: 'mulq', 0x1D: 'maxi', 0x1E: 'muli', 0x1F: 'minii', 0x20: 'addq', 0x21: 'maddq', 0x22: 'addi',
              0x23: 'maddi', 0x24: 'subq', 0x25: 'msubq', 0x26: 'subi', 0x27: 'msubi', 0x28: 'add', 0x29: 'madd',
              0x2A: 'mul', 0x2B: 'max', 0x2C: 'sub', 0x2D: 'msub', 0x2E: 'opmsub', 0x2F: 'mini'}
    if op in simple:
        n = simple[op]
        src = {'q': 'Q', 'i': 'I'}.get(n[-1], '') if op < 0x28 else ''
        if src:
            return f"{n}.{d} vf{fd}, vf{fs}, {src}{flags}"
        return f"{n}.{d} vf{fd}, vf{fs}, vf{ft}{flags}"
    if op >= 0x3C:
        sp = (w & 3) | ((w >> 4) & 0x7C)
        acc = {0x00: 'adda', 0x04: 'suba', 0x08: 'madda', 0x0C: 'msuba', 0x18: 'mula'}
        if sp < 0x10 or (0x18 <= sp < 0x1C):
            n = acc[sp & ~3]
            return f"{n}{BC[sp & 3]}.{d} ACC, vf{fs}, vf{ft}{BC[sp & 3]}{flags}"
        conv = {0x10: 'itof0', 0x11: 'itof4', 0x12: 'itof12', 0x13: 'itof15', 0x14: 'ftoi0', 0x15: 'ftoi4',
                0x16: 'ftoi12', 0x17: 'ftoi15'}
        if sp in conv:
            return f"{conv[sp]}.{d} vf{ft}, vf{fs}{flags}"
        sp2 = {0x1C: 'mulaq', 0x1D: 'abs', 0x1E: 'mulai', 0x1F: 'clipw', 0x20: 'addaq', 0x21: 'maddaq',
               0x22: 'addai', 0x23: 'maddai', 0x24: 'subaq', 0x25: 'msubaq', 0x26: 'subai', 0x27: 'msubai',
               0x28: 'adda', 0x29: 'madda', 0x2A: 'mula', 0x2C: 'suba', 0x2D: 'msuba', 0x2E: 'opmula', 0x2F: 'nop'}
        if sp in sp2:
            n = sp2[sp]
            if n == 'nop':
                return 'nop' + flags
            if n == 'abs':
                return f"abs.{d} vf{ft}, vf{fs}{flags}"
            if n == 'clipw':
                return f"clipw.xyz vf{fs}, vf{ft}w{flags}"
            src = {'q': 'Q', 'i': 'I'}.get(n[-1], '') if sp < 0x28 else ''
            if src:
                return f"{n}.{d} ACC, vf{fs}, {src}{flags}"
            return f"{n}.{d} ACC, vf{fs}, vf{ft}{flags}"
    return f".word 0x{w:08x}{flags}"


def lower(w, pc):
    if w == 0x8000033C:
        return 'nop'
    op = w >> 25
    d = DEST((w >> 21) & 15)
    it, is_, id_ = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    imm11 = w & 0x7FF
    if imm11 & 0x400:
        imm11 -= 0x800
    imm15 = (w & 0x7FF) | ((w >> 10) & 0x7800)
    tgt = (pc + 8 + imm11 * 8) & 0x3FFF
    branches = {0x20: 'b', 0x21: 'bal', 0x28: 'ibeq', 0x29: 'ibne', 0x2C: 'ibltz', 0x2D: 'ibgtz', 0x2E: 'iblez',
                0x2F: 'ibgez'}
    if op in branches:
        n = branches[op]
        if n == 'b':
            return f"b 0x{tgt:04x}"
        if n == 'bal':
            return f"bal vi{it}, 0x{tgt:04x}"
        if n in ('ibeq', 'ibne'):
            return f"{n} vi{it}, vi{is_}, 0x{tgt:04x}"
        return f"{n} vi{is_}, 0x{tgt:04x}"
    if op == 0x24:
        return f"jr vi{is_}"
    if op == 0x25:
        return f"jalr vi{it}, vi{is_}"
    if op == 0x00:
        return f"lq.{d} vf{it}, {imm11}(vi{is_})"
    if op == 0x01:
        return f"sq.{d} vf{is_}, {imm11}(vi{it})"
    if op == 0x04:
        return f"ilw.{d} vi{it}, {imm11}(vi{is_})"
    if op == 0x05:
        return f"isw.{d} vi{it}, {imm11}(vi{is_})"
    if op == 0x08:
        return f"iaddiu vi{it}, vi{is_}, 0x{imm15:x}"
    if op == 0x09:
        return f"isubiu vi{it}, vi{is_}, 0x{imm15:x}"
    if op == 0x10:
        return f"fceq vi1, 0x{w & 0xFFFFFF:06x}"
    if op == 0x11:
        return f"fcset 0x{w & 0xFFFFFF:06x}"
    if op == 0x12:
        return f"fcand vi1, 0x{w & 0xFFFFFF:06x}"
    if op == 0x13:
        return f"fcor vi1, 0x{w & 0xFFFFFF:06x}"
    if op == 0x14:
        return f"fseq vi{it}, 0x{(w & 0x7FF) | ((w >> 10) & 0x800):x}"
    if op == 0x15:
        return f"fsset 0x{(w & 0x7FF) | ((w >> 10) & 0x800):x}"
    if op == 0x16:
        return f"fsand vi{it}, 0x{(w & 0x7FF) | ((w >> 10) & 0x800):x}"
    if op == 0x17:
        return f"fsor vi{it}, 0x{(w & 0x7FF) | ((w >> 10) & 0x800):x}"
    if op == 0x18:
        return f"fmeq vi{it}, vi{is_}"
    if op == 0x1A:
        return f"fmand vi{it}, vi{is_}"
    if op == 0x1B:
        return f"fmor vi{it}, vi{is_}"
    if op == 0x1C:
        return f"fcget vi{it}"
    if op == 0x40:
        low = w & 0x3F
        if low == 0x30:
            return f"iadd vi{id_}, vi{is_}, vi{it}"
        if low == 0x31:
            return f"isub vi{id_}, vi{is_}, vi{it}"
        if low == 0x32:
            imm5 = id_ - 32 if id_ & 16 else id_
            return f"iaddi vi{it}, vi{is_}, {imm5}"
        if low == 0x34:
            return f"iand vi{id_}, vi{is_}, vi{it}"
        if low == 0x35:
            return f"ior vi{id_}, vi{is_}, vi{it}"
        if low >= 0x3C:
            sp = (w & 3) | ((w >> 4) & 0x7C)
            fsf, ftf = BC[(w >> 21) & 3], BC[(w >> 23) & 3]
            m = {0x30: f"move.{d} vf{it}, vf{is_}", 0x31: f"mr32.{d} vf{it}, vf{is_}",
                 0x34: f"lqi.{d} vf{it}, (vi{is_}++)", 0x35: f"sqi.{d} vf{is_}, (vi{it}++)",
                 0x36: f"lqd.{d} vf{it}, (--vi{is_})", 0x37: f"sqd.{d} vf{is_}, (--vi{it})",
                 0x38: f"div Q, vf{is_}{fsf}, vf{it}{ftf}", 0x39: f"sqrt Q, vf{it}{ftf}",
                 0x3A: f"rsqrt Q, vf{is_}{fsf}, vf{it}{ftf}", 0x3B: 'waitq',
                 0x3C: f"mtir vi{it}, vf{is_}{fsf}", 0x3D: f"mfir.{d} vf{it}, vi{is_}",
                 0x3E: f"ilwr.{d} vi{it}, (vi{is_})", 0x3F: f"iswr.{d} vi{it}, (vi{is_})",
                 0x40: f"rnext.{d} vf{it}, R", 0x41: f"rget.{d} vf{it}, R", 0x42: f"rinit R, vf{is_}{fsf}",
                 0x43: f"rxor R, vf{is_}{fsf}", 0x64: f"mfp.{d} vf{it}, P", 0x68: f"xtop vi{it}",
                 0x69: f"xitop vi{it}", 0x6C: f"xgkick vi{is_}",
                 0x70: f"esadd P, vf{is_}", 0x71: f"ersadd P, vf{is_}", 0x72: f"eleng P, vf{is_}",
                 0x73: f"erleng P, vf{is_}", 0x74: f"eatanxy P, vf{is_}", 0x75: f"eatanxz P, vf{is_}",
                 0x76: f"esum P, vf{is_}", 0x78: f"esqrt P, vf{is_}{fsf}", 0x79: f"ersqrt P, vf{is_}{fsf}",
                 0x7A: f"ercpr P, vf{is_}{fsf}", 0x7B: 'waitp', 0x7C: f"esin P, vf{is_}{fsf}",
                 0x7D: f"eatan P, vf{is_}{fsf}", 0x7E: f"eexp P, vf{is_}{fsf}"}
            if sp in m:
                return m[sp]
    if op == 0x02 or (op >> 1) == 0x00:
        pass
    return f".word 0x{w:08x}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--offset', type=lambda x: int(x, 0), default=0)
    ap.add_argument('--pairs', type=lambda x: int(x, 0), default=256)
    ap.add_argument('--base', type=lambda x: int(x, 0), default=0)
    a = ap.parse_args()
    data = open(a.file, 'rb').read()
    for i in range(a.pairs):
        o = a.offset + i * 8
        if o + 8 > len(data):
            break
        lo, up = struct.unpack_from('<II', data, o)
        pc = a.base + i * 8
        print(f"{pc:04x}: {upper(up):44s} {lower(lo, pc)}")


if __name__ == '__main__':
    main()
