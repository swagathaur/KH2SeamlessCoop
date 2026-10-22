#!/usr/bin/env python3
"""Regenerate all translated C++ from the user's own game files.

Steps (run from the repository root after tools/extract_game.py):
  1. wrap each overlay image as an ELF        -> build/overlays/<name>.elf
  2. ps2_recomp config/kh2.toml               -> generated/main/
  3. ps2_recomp config/overlays/<name>.toml   -> generated/ovl_<name>/
  4. tools/gen_overlay_tables.py              -> overlay dispatch tables

After each ps2_recomp run the output is post-processed (slim_includes,
forward_replaced) in a staging directory and only changed files are copied.

Usage: recomp.py --recompiler PATH [--only main|overlays]
"""
import argparse
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OVERLAYS = ["title", "movie", "shop", "gumimenu", "gumibattle"]


def run(cmd, log=None):
    t = time.time()
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("w", encoding="utf-8") as fh:
            code = subprocess.run(cmd, cwd=ROOT, stdout=fh, stderr=subprocess.STDOUT).returncode
    else:
        code = subprocess.run(cmd, cwd=ROOT).returncode
    return code, time.time() - t


DECL_RE = re.compile(r"^void (\w+)\(uint8_t\* ?rdram, R5900Context\* ?ctx, PS2Runtime ?\* ?runtime\);$", re.M)
NAME_RE = re.compile(r"\b(\w+_0x[0-9a-f]+)\b")
MACROS_INCLUDE = '#include "ps2_runtime_macros.h"\n'
VU_SATURATE_INCLUDE = '#include "ps2_vu_saturate.h"\n'
GLOBAL_INCLUDES = ('#include "ps2_recompiled_functions.h"\n', '#include "ps2_recompiled_stubs.h"\n')


def slim_includes(tree: Path):
    """Replace the all-functions headers in each generated file with
    declarations of just the functions that file names.

    ps2_recomp has every file include headers declaring every function, so
    adding one function changed every file's dependencies and forced a full
    rebuild (~18k objects). Calls mostly go through the runtime by address;
    a typical file names no other function at all.
    """
    decls = {}
    for header in ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h"):
        h = tree / header
        if h.exists():
            for name in DECL_RE.findall(h.read_text(encoding="utf-8")):
                decls[name] = f"void {name}(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);"
    for cpp in tree.glob("*.cpp"):
        if cpp.name == "register_functions.cpp" or cpp.name.startswith("overlay_table_"):
            continue
        text = cpp.read_text(encoding="utf-8")
        if GLOBAL_INCLUDES[0] not in text:
            continue
        used = sorted(set(NAME_RE.findall(text)) & decls.keys())
        block = "".join(f"{decls[n]}\n" for n in used)
        text = text.replace(GLOBAL_INCLUDES[0], "// declarations (tools/recomp.py slim_includes)\n" + block, 1)
        text = text.replace(GLOBAL_INCLUDES[1], "", 1)
        # Saturating VU0 arithmetic (patches/ps2recomp/0022): only the files
        # that use it include the header, so it needs no full rebuild.
        if "_SAT(" in text and VU_SATURATE_INCLUDE not in text:
            text = text.replace(MACROS_INCLUDE, MACROS_INCLUDE + VU_SATURATE_INCLUDE, 1)
        cpp.write_text(text, encoding="utf-8")


SIG_RE = re.compile(r"^void (\w+_0x([0-9a-f]+))\(uint8_t\* ?rdram, R5900Context\* ?ctx, PS2Runtime ?\* ?runtime\) \{$", re.M)
STUB_CALL_RE = re.compile(r"^    ps2_(stubs|syscalls)::\w+\(rdram, ctx, runtime\);", re.M)
FORWARD = """    {{ // tools/recomp.py: a tail jump (J) calls this directly; honour PS2Runtime::replaceFunction
        const bool viaOriginal = kh2_callingOriginal; // set by kh2::callOriginal (port/runtime/hle_call.h)
        kh2_callingOriginal = false;
        const PS2Runtime::RecompiledFunction replacement =
            g_ps2RecompiledFunctionTable[(0x{addr}u - g_ps2RecompiledFunctionTableBase) >> 2];
        if (!viaOriginal && replacement && replacement != &{name})
        {{
            ctx->pc = 0x{addr}u;
            replacement(rdram, ctx, runtime);
            return;
        }}
    }}
"""


def hle_addresses():
    path = ROOT / "config" / "hle_functions.txt"
    out = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.add(int(line.split()[0], 16))
    return out


def forward_replaced(tree: Path, hle: set):
    """Make stub wrappers and HLE-replaced functions defer to the runtime's
    function table, so a replacement installed at run time is reached even
    through a tail jump, which the recompiler emits as a direct call."""
    count = 0
    for cpp in tree.glob("*.cpp"):
        text = cpp.read_text(encoding="utf-8")
        m = SIG_RE.search(text)
        if not m:
            continue
        name, addr = m.group(1), int(m.group(2), 16)
        if addr not in hle and not STUB_CALL_RE.search(text):
            continue
        insert = FORWARD.format(addr=f"{addr:x}", name=name)
        text = text[:m.end()] + "\n" + insert.rstrip("\n") + text[m.end():]
        text = text[:m.start()] + "extern thread_local bool kh2_callingOriginal;\n" + text[m.start():]
        cpp.write_text(text, encoding="utf-8")
        count += 1
    return count


def sync_tree(src: Path, dst: Path):
    """Copy changed files from src to dst and delete files gone from src.

    Unchanged files keep their timestamps, so the build only recompiles what
    actually changed (a full rebuild of the main ELF is ~18k objects).
    """
    dst.mkdir(parents=True, exist_ok=True)
    changed = 0
    new = {p.name for p in src.iterdir() if p.is_file()}
    for name in new:
        a, b = src / name, dst / name
        if not b.exists() or a.stat().st_size != b.stat().st_size or a.read_bytes() != b.read_bytes():
            shutil.copy2(a, b)
            changed += 1
    removed = 0
    for p in dst.iterdir():
        if p.is_file() and p.name not in new and not p.name.startswith("overlay_table_"):
            p.unlink()
            removed += 1
    return changed, removed


def recompile(recompiler, config, outdir, label):
    staging = ROOT / "build" / "recomp-staging" / label
    if staging.exists():
        shutil.rmtree(staging)
    text = (ROOT / config).read_text(encoding="utf-8")
    staged_config = ROOT / "build" / "recomp-staging" / f"{label}.toml"
    staged_config.parent.mkdir(parents=True, exist_ok=True)
    staged_config.write_text(text.replace(f'output = "{outdir}"',
                                          f'output = "build/recomp-staging/{label}"'), encoding="utf-8")
    log = ROOT / "logs" / f"recomp_{label}.log"
    code, secs = run([recompiler, str(staged_config.relative_to(ROOT))], log)
    files = list(staging.glob("*.cpp")) if staging.exists() else []
    if code == 0 and files:
        slim_includes(staging)
        if label == "main":
            print(f"{label:11} {forward_replaced(staging, hle_addresses())} functions forward to replacements")
        changed, removed = sync_tree(staging, ROOT / outdir)
        print(f"{label:11} {changed} files changed, {removed} removed")
    summary = [l.strip() for l in log.read_text(encoding="utf-8", errors="replace").splitlines()
               if "[error]" in l][:5]
    print(f"{label:11} exit {code}  {secs:5.1f}s  {len(files):6} files")
    for line in summary:
        print("    " + line[:200])
    return code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler", required=True)
    ap.add_argument("--only", choices=["main", "overlays"])
    a = ap.parse_args()
    recompiler = str(Path(a.recompiler).resolve())
    failed = 0
    if a.only != "overlays":
        failed |= recompile(recompiler, "config/kh2.toml", "generated/main", "main")
    if a.only != "main":
        for name in OVERLAYS:
            code, _ = run([sys.executable, "tools/overlay_to_elf.py", f"game/overlays/ovl_{name}.x",
                           "-o", f"build/overlays/{name}.elf"])
            failed |= code
            failed |= recompile(recompiler, f"config/overlays/{name}.toml", f"generated/ovl_{name}", name)
        code, _ = run([sys.executable, "tools/gen_overlay_tables.py"])
        failed |= code
    if a.only != "overlays" and not failed:
        code, _ = run([sys.executable, "tools/game_patches.py", "--recompiler", recompiler])
        failed |= code
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
