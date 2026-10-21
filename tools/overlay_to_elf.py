#!/usr/bin/env python3
"""Wrap a KH2 EE code overlay (ovl_*.x) in a minimal MIPS ELF.

KH2 Final Mix keeps five subsystems out of the main executable and loads them
on demand into a fixed arena. The decompressed files are flat images: no
header, linked to run at 0x01F18000 (the address of the main ELF's `overlay`
section). Before loading, the game zeroes 0x01F18000..0x01FC0000
(FUN_00168548), so anything past the image is implicit .bss.

The image does not say where code ends and data begins, so the whole file is
emitted as one executable PROGBITS section; Ghidra and ps2_recomp only decode
what they discover as functions. Approach adapted from DQ8Recomp's
tools/mwo3/mwo3_to_elf.py (GPL-3.0).

Usage: overlay_to_elf.py game/overlays/ovl_title.x -o build/overlays/title.elf
"""
import argparse
import struct
from pathlib import Path

ARENA_START = 0x01F18000
ARENA_END = 0x01FC0000
E_FLAGS = 0x20920001  # same as SLPM_666.75: noreorder, R5900, mips3

EM_MIPS, ET_EXEC, EV_CURRENT, PT_LOAD = 8, 2, 1, 1
PF_X, PF_W, PF_R = 1, 2, 4
SHT_PROGBITS, SHT_NOBITS, SHT_STRTAB = 1, 8, 3
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4


def build_elf(image: bytes, entry: int) -> bytes:
    if len(image) % 4:
        image += b"\0" * (4 - len(image) % 4)
    bss_addr = ARENA_START + len(image)
    bss_size = ARENA_END - bss_addr
    if bss_size < 0:
        raise SystemExit("overlay image is larger than the arena")

    names = ["", ".text", ".bss", ".shstrtab"]
    shstrtab = b"\0".join(n.encode() for n in names) + b"\0"
    name_off, cur = {}, 0
    for n in names:
        name_off[n] = cur
        cur += len(n) + 1

    ehsize, phentsize, shentsize, phnum = 52, 32, 40, 2
    text_off = 0x1000                     # page-aligned, like the main ELF
    shstr_off = text_off + len(image)
    shoff = (shstr_off + len(shstrtab) + 3) & ~3

    eh = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    eh += struct.pack("<HHIIIIIHHHHHH", ET_EXEC, EM_MIPS, EV_CURRENT, entry, ehsize, shoff,
                      E_FLAGS, ehsize, phentsize, phnum, shentsize, len(names), 3)
    ph = struct.pack("<8I", PT_LOAD, text_off, ARENA_START, ARENA_START, len(image), len(image),
                     PF_R | PF_W | PF_X, 0x1000)
    ph += struct.pack("<8I", PT_LOAD, 0, bss_addr, bss_addr, 0, bss_size, PF_R | PF_W, 0x1000)

    sh = b"\0" * shentsize
    sh += struct.pack("<10I", name_off[".text"], SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR | SHF_WRITE,
                      ARENA_START, text_off, len(image), 0, 0, 16, 0)
    sh += struct.pack("<10I", name_off[".bss"], SHT_NOBITS, SHF_ALLOC | SHF_WRITE,
                      bss_addr, shstr_off, bss_size, 0, 0, 16, 0)
    sh += struct.pack("<10I", name_off[".shstrtab"], SHT_STRTAB, 0, 0, shstr_off, len(shstrtab), 0, 0, 1, 0)

    out = bytearray(eh + ph)
    out += b"\0" * (text_off - len(out))
    out += image + shstrtab
    out += b"\0" * (shoff - len(out))
    out += sh
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("overlay")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    image = Path(a.overlay).read_bytes()
    # Offset 0x14 holds `j <entry>`; use its target as the ELF entry point.
    word = struct.unpack_from("<I", image, 0x14)[0]
    entry = ((ARENA_START + 0x14) & 0xF0000000) | ((word & 0x03FFFFFF) << 2) if word >> 26 == 2 else ARENA_START + 0x14
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_bytes(build_elf(image, entry))
    print(f"{a.out}: {len(image):#x} bytes at {ARENA_START:#010x}, entry {entry:#010x}")


if __name__ == "__main__":
    main()
