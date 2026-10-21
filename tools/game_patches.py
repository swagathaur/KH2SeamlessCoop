#!/usr/bin/env python3
"""Build the switchable game patches in config/game_patches.toml.

For code patches, the whole ELF is recompiled from a patched copy (with the
normal function map; it takes seconds) and the functions containing patched
instructions are kept and renamed, so both versions exist
in the build; port/runtime/game_patches.cpp swaps the patched ones in when a
patch is enabled (--patch=NAME). Data patches become a table of writes.

Output: generated/patches/*.cpp (game-derived, never committed), including
game_patch_table.cpp. Run after tools/recomp.py (which calls this itself).

Usage: game_patches.py --recompiler PATH
"""
import argparse
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

try:
    import tomllib
except ImportError:  # Python < 3.11
    sys.exit("game_patches.py needs Python 3.11 or newer (tomllib)")

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import recomp  # noqa: E402  (slim_includes, SIG_RE)

ELF = ROOT / "game" / "SLPM_666.75"
WORK = ROOT / "build" / "game-patches"
OUT = ROOT / "generated" / "patches"


def load_patches():
    data = tomllib.loads((ROOT / "config" / "game_patches.toml").read_text(encoding="utf-8"))
    patches = []
    for name, p in data.items():
        def entries(key):
            out = []
            for e in p.get(key, []):
                addr, value = int(e[0]), int(e[1])
                bits = int(e[2]) if len(e) > 2 else 32
                if bits not in (8, 16, 32):
                    sys.exit(f"{name}: bad width {bits}")
                out.append((addr, value, bits))
            return out
        patches.append({"name": name, "description": p.get("description", ""), "author": p.get("author", ""),
                        "aspect": float(p.get("aspect", 0.0)), "code": entries("code"), "data": entries("data")})
    return patches


def elf_segments(blob):
    phoff = struct.unpack_from("<I", blob, 0x1C)[0]
    count = struct.unpack_from("<H", blob, 0x2C)[0]
    segs = []
    for i in range(count):
        p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from("<8I", blob, phoff + i * 32)
        if p_type == 1:
            segs.append((p_vaddr, p_offset, p_filesz))
    return segs


def vaddr_to_offset(segs, addr):
    for vaddr, off, size in segs:
        if vaddr <= addr < vaddr + size:
            return addr - vaddr + off
    sys.exit(f"address 0x{addr:08X} is not in the ELF's loaded image")


def load_function_map():
    funcs = []
    for line in (ROOT / "config" / "functions.recomp.csv").read_text(encoding="utf-8").splitlines()[1:]:
        name, start, end, _ = line.split(",")
        funcs.append((int(start, 16), int(end, 16), name))
    return funcs


def containing(funcs, addr):
    for start, end, name in funcs:
        if start <= addr < end:
            return start, end, name
    sys.exit(f"no function contains patched address 0x{addr:08X}")


def ident(name):
    return re.sub(r"\W", "_", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler", required=True)
    a = ap.parse_args()
    patches = load_patches()
    funcs = load_function_map()
    blob = bytearray(ELF.read_bytes())
    segs = elf_segments(blob)

    # Every code patch's functions, and which patch owns each. Two patches may
    # not touch the same function (they would need combined variants).
    owner = {}
    for p in patches:
        p["functions"] = []
        for addr, value, bits in p["code"]:
            start, end, name = containing(funcs, addr)
            if start in owner and owner[start] != p["name"]:
                sys.exit(f"{p['name']} and {owner[start]} both patch {name}")
            owner[start] = p["name"]
            if start not in [f[0] for f in p["functions"]]:
                p["functions"].append((start, end, name))
            off = vaddr_to_offset(segs, addr)
            if bits == 32:
                struct.pack_into("<I", blob, off, value & 0xFFFFFFFF)
            elif bits == 16:
                struct.pack_into("<H", blob, off, value & 0xFFFF)
            else:
                blob[off] = value & 0xFF

    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True)
    staging = WORK / "out"
    variants = {}
    if owner:
        (WORK / "patched.elf").write_bytes(bytes(blob))
        config = (ROOT / "config" / "kh2.toml").read_text(encoding="utf-8")
        config = config.replace('input = "game/SLPM_666.75"', 'input = "build/game-patches/patched.elf"')
        config = config.replace('output = "generated/main"', 'output = "build/game-patches/out"')
        (WORK / "patches.toml").write_text(config, encoding="utf-8")
        log = WORK / "recomp.log"
        with log.open("w", encoding="utf-8") as fh:
            code = subprocess.run([a.recompiler, str((WORK / "patches.toml").relative_to(ROOT))], cwd=ROOT,
                                  stdout=fh, stderr=subprocess.STDOUT).returncode
        if code != 0:
            sys.exit(f"ps2_recomp failed (exit {code}), see {log}")
        recomp.slim_includes(staging)
        for cpp in staging.glob("*.cpp"):
            text = cpp.read_text(encoding="utf-8")
            m = recomp.SIG_RE.search(text)
            if not m or int(m.group(2), 16) not in owner:
                continue
            addr = int(m.group(2), 16)
            new = f"kh2patch_{ident(owner[addr])}_0x{addr:x}"
            variants[addr] = (new, cpp.name, re.sub(rf"\b{re.escape(m.group(1))}\b", new, text))
        missing = sorted(set(owner) - set(variants))
        if missing:
            sys.exit("ps2_recomp produced no code for " + ", ".join(f"0x{a:08X}" for a in missing))

    OUT.mkdir(parents=True, exist_ok=True)
    wanted = {}
    for addr, (new, _, text) in variants.items():
        wanted[f"{new}.cpp"] = text
    table = ['// Generated by tools/game_patches.py from config/game_patches.toml. Do not edit.',
             '#include "ps2_runtime.h"', '#include "game_patch_table.h"', '']
    for new, _, _ in variants.values():
        table.append(f"void {new}(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);")
    table.append("")
    table.append("namespace kh2patches {")
    for p in patches:
        n = ident(p["name"])
        table.append(f"static const CodeVariant kCode_{n}[] = {{")
        for start, _, _ in p["functions"]:
            table.append(f"    {{0x{start:08X}u, &{variants[start][0]}}},")
        table.append("    {0u, nullptr}};")
        table.append(f"static const DataWrite kData_{n}[] = {{")
        for addr, value, bits in p["data"]:
            table.append(f"    {{0x{addr:08X}u, 0x{value:X}u, {bits}}},")
        table.append("    {0u, 0u, 0}};")
    table.append("const GamePatch kGamePatches[] = {")
    for p in patches:
        n = ident(p["name"])
        desc = p["description"].replace('"', '\\"')
        author = p["author"].replace('"', '\\"')
        table.append(f'    {{"{p["name"]}", "{desc}", "{author}", {p["aspect"]}f, kCode_{n}, kData_{n}}},')
    table.append("    {nullptr, nullptr, nullptr, 0.0f, nullptr, nullptr}};")
    table.append("} // namespace kh2patches")
    wanted["game_patch_table.cpp"] = "\n".join(table) + "\n"

    changed = 0
    for name, text in wanted.items():
        path = OUT / name
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8")
            changed += 1
    for path in OUT.glob("*.cpp"):
        if path.name not in wanted:
            path.unlink()
    print(f"game patches: {len(patches)} patches, {len(variants)} patched functions, {changed} files changed")


if __name__ == "__main__":
    main()
