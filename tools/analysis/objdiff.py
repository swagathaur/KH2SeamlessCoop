#!/usr/bin/env python3
"""Compare game objects in a RAM dump (--ram-dump): walks the object list
(head 0x01D48DA0, next at +0x8B4) and prints the words that differ between a
reference object and others, e.g. a native NPC and a spawned copy of the same
entry. Pointers into the objects' own bodies are compared by offset.

Usage: objdiff.py ram.bin [REF_ADDR OTHER_ADDR...] [--size 0xB40] [--list]
With only the dump, lists the objects."""
import struct, sys

LIST_HEAD = 0x01D48DA0
NEXT = 0x8B4

def main():
    args = sys.argv[1:]
    size = 0xB40
    if "--size" in args:
        i = args.index("--size")
        size = int(args[i + 1], 16)
        del args[i:i + 2]
    ram = open(args[0], "rb").read()
    u32 = lambda a: struct.unpack_from("<I", ram, a & 0x1FFFFFF)[0]
    f32 = lambda a: struct.unpack_from("<f", ram, a & 0x1FFFFFF)[0]

    objs = []
    o = u32(LIST_HEAD)
    while o and len(objs) < 200:
        entry = u32(o + 8)
        eid = u32(entry) if entry else 0
        name = ram[(entry + 8) & 0x1FFFFFF:(entry + 0x28) & 0x1FFFFFF].split(b"\0")[0].decode("latin1") if entry else "?"
        objs.append((o, eid, name))
        o = u32(o + NEXT)
    if len(args) == 1:
        for o, eid, name in objs:
            pos = [f32(o + 0x540 + 4 * i) for i in range(3)]
            print(f"{o:08X} {eid & 0xFFFF:04X} {name:24s} vt={u32(o):08X} pos={pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f}")
        return

    addrs = [int(a, 16) for a in args[1:]]
    ref = addrs[0]
    def norm(base, v):
        # A pointer into the object itself: show it as an offset.
        return ("self+%X" % (v - base)) if base <= v < base + size else "%08X" % v
    print("offset   " + "  ".join(f"{a:08X}".rjust(12) for a in addrs))
    for off in range(0, size, 4):
        vals = [norm(a, u32(a + off)) for a in addrs]
        if any(v != vals[0] for v in vals[1:]):
            print(f"+{off:04X}    " + "  ".join(v.rjust(12) for v in vals))

if __name__ == "__main__":
    main()
