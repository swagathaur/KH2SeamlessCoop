# KH2FM Co-op Recomp

A native PC port of **Kingdom Hearts II Final Mix** (PS2, Japan, `SLPM_666.75`),
built by static recompilation with [PS2Recomp](https://github.com/ran-j/PS2Recomp),
as the base for a **3-player online co-op mod**.

> No game code or data is in this repository. You need your own copy of the disc.

## Approach

1. **Recompile the whole game.** PS2Recomp translates every MIPS function in the
   ELF to C++ so the game runs natively without a finished decompilation.
2. **Name things with the decomp.** [GovanifY/kh2](https://github.com/GovanifY/kh2)
   targets this exact ELF and recovered symbols and class layouts from a PS3 build.
   Those names go into Ghidra before the function map is exported.
3. **Replace only what co-op needs.** PS2Recomp game overrides redirect individual
   function addresses to hand-written C++ (party AI, damage, enemy AI, room loading).

Co-op model: every player runs their own instance as "player 1"; other players
appear as networked puppets in party slots. Enemies and world state are
host-authoritative. See [docs/architecture.md](docs/architecture.md).

## Setup

```sh
git submodule update --init --recursive        # fetch PS2Recomp and the decomp
python tools/extract_game.py "Src/<your disc>.iso"
```

`extract_game.py` verifies the ELF's SHA-256 against the decomp's target and writes
everything into `game/` (ignored by git).

## Layout

| Path | Contents |
|---|---|
| `Src/` | Your disc image (ignored) |
| `tools/` | Disc extraction and analysis scripts (Python, no dependencies) |
| `config/` | PS2Recomp TOML config and Ghidra function map |
| `generated/` | Recompiler output (ignored; regenerate from your ELF) |
| `port/runtime/` | Game-specific runtime: entry, overlay loading, renderer glue |
| `port/coop/` | Co-op: overrides, puppets, netcode |
| `third_party/` | Pinned submodules |
| `docs/` | Findings and design |

## Roadmap

| Phase | Goal | Done when |
|---|---|---|
| 0 | Toolchain | ELF in Ghidra with decomp symbols; PS2Recomp builds; first full recompile compiles |
| 1 | Boot | Native build reaches the title screen with sound |
| 2 | Playable | Twilight Town playable end to end at full speed |
| 3 | Hook layer | ImGui overlay listing live actors; first function override working |
| 4 | Ghost | Two instances on a LAN; one player's Sora shown as a puppet in the other's party |
| 5+ | Co-op | Enemy sync, combat events, room transitions, cutscenes |

Current status: **Phase 0** — disc surveyed, ELF verified. See [docs/findings.md](docs/findings.md).
