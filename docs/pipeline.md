# Pipeline: from disc to native build

Everything game-derived is regenerated locally from your own disc. Committed
files are scripts, configs and function maps (addresses and names only).

| Step | Command | Output |
|---|---|---|
| 1. Extract | `python tools/extract_game.py "Src/<disc>.iso"` | `game/`: ELF (hash-checked), IRX modules, indexes, decompressed overlays |
| 2. Build recompiler tools | see below | `build/ps2recomp-tools/.../ps2_recomp(.exe)`, `ps2_analyzer(.exe)` |
| 3. Analyze SDK functions | `ps2_analyzer game/SLPM_666.75 config/analyzer/analyzer.toml` | SDK signature matches, MMIO sites |
| 4. Main function map | `python tools/make_function_map.py third_party/kh2-decomp config/functions.csv config/analyzer/analyzer.toml game/SLPM_666.75 title movie shop gumimenu gumibattle` (after step 6) | `config/functions.csv` |
| 5. Stub list | `python tools/gen_stubs.py config/analyzer/analyzer.toml third_party/PS2Recomp/ps2xRuntime/include/ps2_call_list.h > config/stubs.fragment.toml` | vetted SDK bindings |
| 6. Overlay maps | `GHIDRA_HOME=... tools/ghidra/run_overlays.sh` (needs `build/overlays/*.elf` from `tools/overlay_to_elf.py`) | `config/functions_<overlay>.csv` |
| 6b. Check maps | `python tools/check_function_map.py config/functions.csv game/SLPM_666.75` | branches that would fail at runtime |
| 7. Configs | `python tools/make_config.py` | `config/kh2.toml`, `config/overlays/*.toml` |
| 8. Translate | `tools/win/recomp.ps1` (`tools/recomp.py`) | `generated/main`, `generated/ovl_*`, overlay tables |
| 9. Build | `tools/win/build.ps1` | `build/win/kh2recomp.exe` |

Steps 3-7 only need re-running when the tools or maps change; their outputs are
committed. Steps 1, 2, 8 and 9 are what a fresh checkout needs.

## Building the recompiler tools (Windows)

```powershell
. tools\win\vsenv.ps1
cmake -S third_party\PS2Recomp -B build\ps2recomp-tools -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF
cmake --build build\ps2recomp-tools --parallel
```

## Ghidra (step 6 only)

Ghidra 12.1.4 with the matching
[ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded)
release (v2.1.38), and a JDK 21. The main ELF does not need Ghidra: its map
comes from the decomp's `layout.tsv`, which is a Ghidra export of the same ELF.
