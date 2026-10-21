#!/usr/bin/env python3
"""Disassemble PS2 VU microcode (upper/lower instruction pairs).

Usage: vudis.py <code.bin> [start_pair] [count]
       (code images as written by the runtime's VU capture, PS2_VU_CAPTURE_DIR)

Each 64-bit pair is printed as "addr: upper | lower"; addr is the pair index
(the unit VU branch targets and MSCAL use, x8 for bytes).
"""
import struct
import sys

BC = "xyzw"
UPPER = {}
for i, n in enumerate(["ADD", "SUB", "MADD", "MSUB", "MAX", "MINI", "MUL"]):
    for b in range(4):
        UPPER[i * 4 + b] = f"{n}{BC[b]}"
UPPER.update({0x1C: "MULq", 0x1D: "MAXi", 0x1E: "MULi", 0x1F: "MINIi", 0x20: "ADDq", 0x21: "MADDq",
              0x22: "ADDi", 0x23: "MADDi", 0x24: "SUBq", 0x25: "MSUBq", 0x26: "SUBi", 0x27: "MSUBi",
              0x28: "ADD", 0x29: "MADD", 0x2A: "MUL", 0x2B: "MAX", 0x2C: "SUB", 0x2D: "MSUB",
              0x2E: "OPMSUB", 0x2F: "MINI"})
USPECIAL = {}
for i, n in enumerate(["ADDA", "SUBA", "MADDA", "MSUBA"]):
    for b in range(4):
        USPECIAL[i * 4 + b] = f"{n}{BC[b]}"
for b in range(4):
    USPECIAL[0x18 + b] = f"MULA{BC[b]}"
USPECIAL.update({0x10: "ITOF0", 0x11: "ITOF4", 0x12: "ITOF12", 0x13: "ITOF15", 0x14: "FTOI0", 0x15: "FTOI4",
                 0x16: "FTOI12", 0x17: "FTOI15", 0x1C: "MULAq", 0x1D: "ABS", 0x1E: "MULAi", 0x1F: "CLIP",
                 0x20: "ADDAq", 0x21: "MADDAq", 0x22: "ADDAi", 0x23: "MADDAi", 0x24: "SUBAq", 0x25: "MSUBAq",
                 0x26: "SUBAi", 0x27: "MSUBAi", 0x28: "ADDA", 0x29: "MADDA", 0x2A: "MULA", 0x2C: "SUBA",
                 0x2D: "MSUBA", 0x2E: "OPMULA", 0x2F: "NOP"})
LOWER = {0x00: "LQ", 0x01: "SQ", 0x04: "ILW", 0x05: "ISW", 0x08: "IADDIU", 0x09: "ISUBIU", 0x10: "FCEQ",
         0x11: "FCSET", 0x12: "FCAND", 0x13: "FCOR", 0x14: "FSEQ", 0x15: "FSSET", 0x16: "FSAND", 0x17: "FSOR",
         0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR", 0x1C: "FCGET", 0x20: "B", 0x21: "BAL", 0x24: "JR",
         0x25: "JALR", 0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}
LOWER_OP = {0x30: "IADD", 0x31: "ISUB", 0x32: "IADDI", 0x34: "IAND", 0x35: "IOR"}
LSPECIAL = {0x30: "MOVE", 0x31: "MR32", 0x34: "LQI", 0x35: "SQI", 0x36: "LQD", 0x37: "SQD", 0x38: "DIV",
            0x39: "SQRT", 0x3A: "RSQRT", 0x3B: "WAITQ", 0x3C: "MTIR", 0x3D: "MFIR", 0x3E: "ILWR", 0x3F: "ISWR",
            0x40: "RNEXT", 0x41: "RGET", 0x42: "RINIT", 0x43: "RXOR", 0x64: "MFP", 0x68: "XTOP", 0x69: "XITOP",
            0x6C: "XGKICK", 0x70: "ESADD", 0x71: "ERSADD", 0x72: "ELENG", 0x73: "ERLENG", 0x74: "EATANxy",
            0x75: "EATANxz", 0x76: "ESUM", 0x78: "ESQRT", 0x79: "ERSQRT", 0x7A: "ERCPR", 0x7B: "WAITP",
            0x7C: "ESIN", 0x7D: "EATAN", 0x7E: "EEXP"}


def dest(w):
    d = (w >> 21) & 0xF
    return "".join(c for c, bit in zip("xyzw", (8, 4, 2, 1)) if d & bit)


def upper(w):
    flags = "".join(f for f, b in (("[I]", 31), ("[E]", 30), ("[M]", 29), ("[D]", 28), ("[T]", 27)) if w >> b & 1)
    op = w & 0x3F
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    d = dest(w)
    if op >= 0x3C:
        n = USPECIAL.get(((w >> 6) & 31) << 2 | (w & 3), f"?us{w:08x}")
        if n == "NOP":
            return "nop" + flags
        if n.startswith(("ITOF", "FTOI", "ABS")):
            return f"{n}.{d} vf{ft}, vf{fs}{flags}"
        if n == "CLIP":
            return f"CLIPw.xyz vf{fs}, vf{ft}{flags}"
        return f"{n}.{d} ACC, vf{fs}, vf{ft}{flags}"
    n = UPPER.get(op, f"?u{w:08x}")
    return f"{n}.{d} vf{fd}, vf{fs}, vf{ft}{flags}"


def simm(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def lower(w, pc):
    op = w >> 25
    it, is_, imm5 = (w >> 16) & 31, (w >> 11) & 31, simm((w >> 6) & 31, 5)
    imm11 = simm(w & 0x7FF, 11)
    d = dest(w)
    if op == 0x40:
        f = w & 0x3F
        if f >= 0x3C:
            n = LSPECIAL.get(((w >> 6) & 31) << 2 | (w & 3), f"?ls{w:08x}")
            if n in ("XTOP", "XITOP"):
                return f"{n} vi{it}"
            if n == "XGKICK":
                return f"XGKICK vi{is_}"
            if n in ("MTIR",):
                return f"MTIR vi{it}, vf{is_}.{'xyzw'[(w >> 21) & 3]}"
            if n in ("MFIR",):
                return f"MFIR.{d} vf{it}, vi{is_}"
            if n in ("ILWR", "ISWR"):
                return f"{n}.{d} vi{it}, (vi{is_})"
            if n in ("LQI", "SQI", "LQD", "SQD"):
                return f"{n}.{d} vf{it}, vi{is_}"
            if n in ("DIV", "RSQRT"):
                return f"{n} Q, vf{is_}.{'xyzw'[(w >> 21) & 3]}, vf{it}.{'xyzw'[(w >> 23) & 3]}"
            if n == "SQRT":
                return f"SQRT Q, vf{it}.{'xyzw'[(w >> 23) & 3]}"
            if n in ("WAITQ", "WAITP"):
                return n.lower()
            if n in ("MOVE", "MR32"):
                return f"{n}.{d} vf{it}, vf{is_}"
            return f"{n}.{d} vf{it}, vf{is_}"
        n = LOWER_OP.get(f, f"?lo{w:08x}")
        if n == "IADDI":
            return f"IADDI vi{it}, vi{is_}, {imm5}"
        return f"{n} vi{(w >> 6) & 31}, vi{is_}, vi{it}"
    n = LOWER.get(op)
    if n is None:
        return f"?l{w:08x}"
    target = pc + 1 + imm11
    if n in ("LQ", "SQ"):
        return f"{n}.{d} vf{it}, {imm11}(vi{is_})" if n == "LQ" else f"SQ.{d} vf{is_}, {imm11}(vi{it})"
    if n in ("ILW", "ISW"):
        return f"{n}.{d} vi{it}, {imm11}(vi{is_})"
    if n in ("IADDIU", "ISUBIU"):
        imm15 = ((w >> 21) & 0xF) << 11 | (w & 0x7FF)
        return f"{n} vi{it}, vi{is_}, 0x{imm15:x}"
    if n in ("B", "BAL"):
        return f"{n} {'vi%d, ' % it if n == 'BAL' else ''}->{target}"
    if n in ("JR", "JALR"):
        return f"{n} {'vi%d, ' % it if n == 'JALR' else ''}vi{is_}"
    if n in ("IBEQ", "IBNE"):
        return f"{n} vi{it}, vi{is_}, ->{target}"
    if n.startswith("IB"):
        return f"{n} vi{is_}, ->{target}"
    if n.startswith(("FC", "FS", "FM")):
        if n in ("FCGET",):
            return f"FCGET vi{it}"
        if n in ("FMEQ", "FMAND", "FMOR", "FSAND", "FSEQ", "FSOR"):
            return f"{n} vi{it}, vi{is_}" if n.startswith("FM") else f"{n} vi{it}, 0x{w & 0xFFF:x}"
        return f"{n} vi1, 0x{w & 0xFFFFFF:x}"
    return n


def main():
    code = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    count = int(sys.argv[3], 0) if len(sys.argv) > 3 else len(code) // 8 - start
    for pc in range(start, min(start + count, len(code) // 8)):
        lo, up = struct.unpack_from("<II", code, pc * 8)
        if up >> 31 & 1:
            low = f"LOI {struct.unpack('<f', struct.pack('<I', lo))[0]:g}"
        else:
            low = lower(lo, pc)
        print(f"{pc:5d}: {upper(up):42} | {low}")


if __name__ == "__main__":
    main()
