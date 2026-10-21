# KH2FM Co-op Recomp

A native PC port of **Kingdom Hearts II Final Mix** (PS2, Japan, `SLPM_666.75`),
built by static recompilation, as the base for a **3-player online co-op mod**.

> No game code or data is in this repository. You need your own copy of the disc;
> everything game-derived is generated locally into ignored folders.

## Approach

1. **Recompile the whole game.** [PS2Recomp](https://github.com/ran-j/PS2Recomp)
   translates every MIPS function to C++, so the game runs natively without a
   finished decompilation. We use [DQ8Recomp's fork](https://github.com/Sinan-Karakaya/PS2Recomp),
   which adds overlay dispatch, compiled VU1 routines and faster GS rendering.
2. **Name things with the decomp.** [GovanifY/kh2](https://github.com/GovanifY/kh2)
   targets this exact ELF; its Ghidra function map and recovered names seed ours.
3. **Replace only what co-op needs.** Individual guest functions are redirected
   to hand-written C++ (party AI, damage, enemy AI, room loading).

Co-op model: every player runs their own instance as "player 1"; other players
appear as networked puppets in party slots. Enemies and world state are
host-authoritative. See [docs/architecture.md](docs/architecture.md).

## Setup (Windows)

Needs Visual Studio 2022 with C++ tools (its bundled CMake and Ninja are used),
Python 3 and Git.

```powershell
git submodule update --init --recursive
python tools\extract_game.py "Src\<your disc>.iso"   # verifies the ELF, unpacks overlays
# one-time: build ps2_recomp/ps2_analyzer (see .agent job 009 or docs/pipeline.md)
tools\win\recomp.ps1      # translate the game to C++ in generated\
tools\win\build.ps1       # build build\win\kh2recomp.exe
tools\win\run.ps1         # play in a window
```

Command-line testing (headless runs, screenshots, scripted input, exit codes):
see [docs/harness.md](docs/harness.md).

## Layout

| Path | Contents |
|---|---|
| `Src/` | Your disc image (ignored) |
| `game/` | Files extracted from the disc (ignored) |
| `generated/` | Translated C++ (ignored; regenerate with `tools/win/recomp.ps1`) |
| `config/` | Recompiler configs and function maps (main ELF + five overlays) |
| `port/runtime/` | Runner and test harness, overlay manager, KH2-specific HLE (libpad2, CD, libgraph, CD callbacks) and debugging tools (trace, DMA log, profiler) |
| `port/coop/` | Co-op: overrides, puppets, netcode (not started) |
| `patches/ps2recomp/` | Small patches applied to the runtime at configure time |
| `tools/` | Extraction, function-map, config and pipeline scripts |
| `tools/ghidra/` | Headless Ghidra scripts for the overlay function maps |
| `tools/win/` | Windows build/run scripts and the agent job runner |
| `docs/` | Findings, pipeline, harness and co-op design |

## Roadmap

| Phase | Goal | Done when |
|---|---|---|
| 0 | Toolchain | Function maps, configs, full recompile and a linked native build ✅ |
| 1 | Boot | Native build reaches the title screen with sound ✅ (New Game and the opening movie run too) |
| 2 | Playable | Twilight Town playable end to end at full speed (in progress) |
| 3 | Hook layer | ImGui overlay listing live actors; first function override working |
| 4 | Ghost | Two instances on a LAN; one player's Sora shown as a puppet in the other's party |
| 5+ | Co-op | Enemy sync, combat events, room transitions, cutscenes |

Status and findings: [docs/findings.md](docs/findings.md); every boot blocker and
its fix, in order: [docs/boot-log.md](docs/boot-log.md).

## Credits and licence

GPL-3.0. Builds on PS2Recomp (GPL-3.0), DQ8Recomp's fork and project structure
(GPL-3.0; adapted files say so in their headers), the GovanifY/kh2 decomp (MIT)
and OpenKH (MIT; the KH2 decompressor is a port of its `Img.Decompress`).
