#!/usr/bin/env python3
"""Regenerate all translated C++ from the user's own game files.

Steps (run from the repository root after tools/extract_game.py):
  1. wrap each overlay image as an ELF        -> build/overlays/<name>.elf
  2. ps2_recomp config/kh2.toml               -> generated/main/
  3. ps2_recomp config/overlays/<name>.toml   -> generated/ovl_<name>/
  4. tools/gen_overlay_tables.py              -> overlay dispatch tables

Usage: recomp.py --recompiler PATH [--only main|overlays]
"""
import argparse
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
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
