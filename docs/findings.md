# Disc and ELF findings

Source disc: *Kingdom Hearts II Final Mix+* (Japan). `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLPM_666.75;1`, `VER = 1.01`, NTSC.

## Main ELF — `SLPM_666.75`

- SHA-256 `d869ce4b8edd61cb978c4ff9ede595fe4d7d7b21cbb96a145275217120d8bc87`
  — **exact match** for the GovanifY/kh2 decomp target.
- Stripped (no symbol table). Entry point `0x0010001C`.
- `main` section: `0x00100000`, size `0x27A4B4` (code + data, ~2.5 MB).
  `.bss` is `0x1AD1FE8` (~27 MB), so the game uses nearly all 32 MB of EE RAM.
- `overlay` section: NOBITS, `0x01F18000`, size `0x98` — the load slot
  for the EE code overlays below. Only the stub is in the ELF.
- 14 `.DVP.overlay` sections: **VU microcode** (the vector-unit programs that feed
  the GS). These are not EE code; they need either VU emulation or a
  higher-level renderer replacement. Expect most early graphics work here.

## Disc archives

| File | Size | Notes |
|---|---|---|
| `KH2.IDX` / `KH2.IMG` | 2.93 GB | Main asset archive |
| `OVL.IDX` / `OVL.IMG` | 1.37 GB | 17 entries: 5 code overlays + 12 large files (likely video) |
| `IOPRP300.IMG` + 8 IRX | — | IOP modules: pad (SIO2/DBC/DS2O), memcard, sound (`LIBSD`), `LIBSSL` |

IDX entries are 16 bytes: `u32 hash32, u16 hash16, u16 block, u32 sector, u32 size`.
`hash32` is CRC-32 of the filename (poly `0x04C11DB7`, MSB-first, init and
xorout `0xFFFFFFFF`), confirmed against all five overlay names. In the five overlay
entries `block` has bit `0x4000` set, which appears to mark compressed data
(not yet confirmed).

## EE code overlays

The ELF loads extra MIPS code at runtime from `OVL.IMG`. All five are compressed
with KH2's archive compression (`tools/kh2_compress.py`, a port of OpenKH's
`Img.Decompress`; stored length is `((block & 0x3FFF) + 1) * 0x800`, the index's
size field is the *decompressed* size). Decompressed, each is a flat image with
no header, linked to run at `0x01F18000` (the main ELF's `overlay` section).

| Overlay | Size | Word at +0x14 (`j entry`) | Functions | Needed for |
|---|---|---|---|---|
| `ovl_title.x` | 96,368 | `j 0x01F192C0` | 145 | **boot** |
| `ovl_movie.x` | 84,020 | `j 0x01F18140` | 275 | videos |
| `ovl_shop.x` | 151,632 | `j 0x01F181B8` | 258 | shops |
| `ovl_gumimenu.x` | 476,168 | `j 0x01F18AC0` | 2,145 | Gummi Ship menus |
| `ovl_gumibattle.x` | 443,896 | `j 0x01F18DA8` | 2,751 | Gummi Ship missions |

Before a load the game zeroes `0x01F18000..0x01FC0000` (`FUN_00168548`), so the
arena is 688 KB with implicit .bss. Each overlay's jump word differs, which is
how `port/runtime/overlay_manager.cpp` tells which one is resident without
hooking the loader. Field and battle code are in the main ELF.

Overlay function maps come from headless Ghidra (`tools/ghidra/run_overlays.sh`):
auto-analysis plus a pass that creates functions at stack-frame prologues, since
overlay entry points are reached only through pointers from the main ELF.

## Memory map (from crt0 at `0x0010001C`)

| Range | Use |
|---|---|
| `0x00100000-0x01E4C868` | code, data, bss |
| `0x01E4C868-0x01F18000` | never referenced by the game; the runner puts the runtime's heap and callback stacks here |
| `0x01F18000-0x01FC0000` | overlay arena |
| `0x01FC0000-0x01FE0000` | C heap (`SetupHeap`) |
| `0x01FE0000-0x02000000` | main thread stack (`SetupThread`) |

PS2Recomp's defaults would have put its callback stacks at the top of RAM, on
KH2's main stack; `patches/ps2recomp/0001-async-callback-stack-range.patch` adds
a setter so the runner can move them.

## Function map (main ELF)

`config/functions.csv` (`tools/make_function_map.py`): boundaries from the decomp's
Ghidra layout (11,286 functions), plus 1,020 found by gap filling (every `.ctors`
target and every stack-frame prologue in an unmapped gap; two static constructors
were otherwise missing and the recompiler refuses to build without them). Names:
181 Sony SDK names from ps2_analyzer's signature matches, 67 from the decomp's
layout, ~2,400 recovered C++ names from the decomp.

165 SDK functions bind to runtime implementations (`tools/gen_stubs.py`, which
keeps the guest's own newlib allocator).

## Controller: libpad2

KH2 uses Sony's DualShock 2 library libpad2 (`scePad2*`, with `DS2O.IRX`), which
PS2Recomp does not implement. Only four game functions use it (init
`FUN_00167b98`, socket `FUN_00167728`, per-frame read `FUN_001678f0`, vibration
`FUN_00167ad0`). From those callers: `GetState` must return 1 for a ready pad;
`Read` returns libpad's report minus its two header bytes (buttons active-low in
bytes 0-1, sticks 2-5); `GetButtonProfile` byte 2 low nibble `0xF` enables the
sticks. `port/runtime/pad2_hle.cpp` implements these from the runtime's host
input. KH2 creates two pad sockets, useful later for local co-op input.

## Sound

The game loads `LIBSD.IRX` and `LIBSSL.IRX` (probably Square's sound library) on
the IOP and streams `bgm/*.bgm`, `*/wave*.wd` and `voice/*.vsb` from `KH2.IMG`.
The fork can run disc IRX modules on its emulated IOP (DQ8 does this); not yet set up.
