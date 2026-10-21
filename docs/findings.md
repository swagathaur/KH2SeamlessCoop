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

## EE code overlays — the main recomp risk

The ELF loads extra MIPS code at runtime. PS2Recomp only translates code it can
see ahead of time, so these must be decompressed, extracted and recompiled
separately, with a dispatcher in the runtime (DQ8Recomp did the same).

| Overlay | Stored size | Purpose (from name) | Needed for |
|---|---|---|---|
| `ovl_title.x` | 96,368 | Title screen | **Phase 1 (boot)** |
| `ovl_movie.x` | 84,020 | Video playback | Phase 1–2 |
| `ovl_shop.x` | 151,632 | Shops | Phase 2 |
| `ovl_gumimenu.x` | 476,168 | Gummi Ship menus | Later |
| `ovl_gumibattle.x` | 443,896 | Gummi Ship missions | Later |

Good news for co-op: field and battle code are not in this list, so they
should be in the main ELF and the decomp's symbols.

## Next steps

1. Implement the KH2 decompressor (documented by OpenKH) and unpack the overlays.
2. Find the overlay loader in the ELF (references to `ovl_title.x`, load address `0x01F18000`).
3. Load the ELF in Ghidra with the Emotion Engine extension; import decomp symbols.
4. Build PS2Recomp, export the function map, run the first recompile.
