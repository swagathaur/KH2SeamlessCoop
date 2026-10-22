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

## Game state and the player actor

`port/coop/game_state.h` reads the game's own state. Most addresses come from
the KH2FM modding community's PCSX2 scripts for this exact build (CRC
F266B00B; KH2FM-Mods-Num/GoA-ROM-Edition, "F266B00B GoA ROM.lua") and check
out at run time (the F3 panel and the harness status line show them):

| Address | What |
|---|---|
| `0x0032BAE0` | Current location: world u8, room u8, door u16, map u16, battle u16, event u16 |
| `0x0032BB30` | Save data in play |
| `0x01C6C750` | Party unit slot 1 (stride `0x268`): HP `+0`, max `+4`, MP `+0x180`, max `+0x184`, drive `+0x1B1`/`+0x1B2` |
| `0x01D48DB8` | Control state (3 in cutscenes) |
| `0x01C61958` | Battle state: 0 none, 1 battle, 2 forced |
| `0x00349E0C` | Game speed (float); the frame delta the game uses is next to it at `0x00349E10` |

**Player actor** (found here by RAM snapshot diffing with `--ram-dump` while
the walk script moved Roxas; `tools/analysis/find_position.py`): the static
`0x00352400` points at the player's `YS::PLAYER` object, whose first word is
the class vtable `0x00352410` (one entry is `YS::PLAYER::_OVR_control`). In
the object: a 4x4 world matrix at `+0x40` (rows; translation at `+0x70`,
y up, 0 at floor level in Market Street), a velocity-like vector at `+0x20`,
scale at `+0x30`, and the facing angle in radians at `+0x55C` (matrix yaw + π,
mirrored at `+0x66C`). Other statics holding the same pointer: `0x00341708`,
`0x00348638`, `0x00348754` (next to the camera type), `0x0034E948`,
`0x0034ED04`, `0x01C5FF10`, `0x01D48DA0`. Running moves about 7.4 units a
frame.

**Object list.** `YS::OBJ::GetPlayer()` (`0x0016B0D0`) returns `*0x0034E948`.
All live objects hang off `0x01D48DA0`, linked through `+0x8B4`
(`0x001B9BC8` is the game's iterator; it skips objects whose `is_leave()` is
set). `+8` points at the object's `00objentry.bin` record (id u32, type u8 at
`+4`, model name at `+8`). Market Street at the start of gameplay:
`P_EX110` (Roxas, type 0), `PRIZE`, `N_EX500/510/520_RTN` (Hayner, Pence,
Olette; type 15), `W_EX010` (his weapon, type 5). The F3 panel lists them.

**Transform.** An object's position is `+0x540` (x, y, z, 1) and its direction
`+0x550` (sin yaw, 0, cos yaw, yaw); `FUN_00193608(obj)` builds the world
matrix (`+0x40`) from them and is called each frame by `YS::OBJ::calc_motion`
(`0x001B9B28`) through `jal`, so the runtime can override it. Calls made with
`j` (tail calls, such as from `YS::OBJ::set_pos`) are compiled as direct C++
calls and cannot be redirected; `jal` and `jalr` go through the runtime's
dispatch. Vtables put `initialize` at `+0x08`, `pre_exec` at `+0x0C`,
`control` at `+0x10` and `leave_req` at `+0x20` (from the decomp's named
`VTABLE<T>` functions); the player's is `0x00352410`.

Tools: `tools/analysis/disasm.py` (capstone; names from the function map),
`callers.py` (jal/j call sites), `find_position.py` (snapshot diffing).

**Spawning (puppets).** `FUN_0016F000(entryId, FVector*
pos, float yaw in $f12)` is the object factory: it switches on the objentry
type (jump table `0x0036C3DC`) and allocates each class from the area heap
(`AREA::Alloc`), with entry `0x236` ("PLAYER") special-cased to Sora's current
form. Room spawns go through `YS::UNIT_DATA_OBJ::appear` (`0x00199938`) →
`FUN_00199890`, which calls the factory and then the object's vtable `+0x38`
with the unit group and spawn record. `00objentry.bin` is in memory at
`*0x01D5BA10` (u32 3, u32 count = 1900, then 0x60-byte entries: id u32, type
u8 at +4, model at +8, motion set at +0x28). (`B_EX110_RTN`, `0x084C`, is Axel, not Roxas: `EX110`
is Roxas only with the `P_` prefix.) Puppets are type-15 objects ("npc, no
collision", the townsfolk's class) whose objentry record is first dressed in
the remote player's model and motion-set names (+0x08 and +0x28, copied from
their own entry, e.g. `0x005A` `P_EX110`); the slots use `0x084C` and
`0x084D` and are put back afterwards.

**Loading an entry's files.** `FUN_0016EB48` / `FUN_0016ED50(entry, -1, 0x1E)`
(what a room's spawn list does, `FUN_00199D38`) only queue reads into
`YS::CACHE_BUFF`; `CACHE_BUFF::Flush(TASK*)` (`0x00170888`) starts them, and
with a null task returns at once (the script's `trap_file_flush`). Without the
flush a spawned object gets no model (+0x7AC, +0x7B4 and its MOTION stay 0)
and is never drawn. Calling the factory
from an override works (it returns an object linked at the list head), but:
`0x0034E948` is not a dedicated player pointer, it is the newest object
constructed (the player just happens to be created last when a room loads),
so a spawn must save and restore it or the camera follows the new object;
and room spawns finish an object with vtable `+0x38` (`0x0016BF38`, shared by
all objects): `(obj, UNIT_GROUP*, UNIT_DATA_OBJ*)`, stored at `+0x818`/`+0x81C`
and read by NPC code every frame. `port/coop/puppets.cpp` passes a zeroed
group and a spawn record (entry, position, yaw at `+0x14`) allocated with
`AREA::Alloc` (`0x001050D8`, the room's heap).

Other factory callers: event code (`FUN_002332A8` and neighbours) ORs
`0x40000000` into the entry id, checks the object made it into the list
(`FUN_001BA028`) and calls `sa::EVENT::setActorParam`; the script trap
`FUN_00170F98` and a reaction-command spawner (`FUN_001C4918`, entry `0x558`,
then `YS::OBJ::set_unit_arg`) call only the factory. Class constructors by
objentry type are in the jump table: type 15 ("npc, no collision", which
`B_EX110_RTN` and the townsfolk are) is `FUN_0018ABA0` on a `0xB40`-byte
object, over the shared base `FUN_001980D0` → `FUN_00169398`.

**Motion.** The object's `YS::MOTION` is at `+0x140`; its `+0x14` (object
`+0x154`) is the motion index: 0 idle, 2 run, 3 jump, 5 attack (Roxas, Market
Street), `+0x0C` points at the current motion's data. `YS::MOTION::change`
(`0x0017F540`, `this` = obj + 0x140, id, `$f12` blend, `$f13` start) switches
it, as the script trap `0x001711B0` does. The current *action* (`+0x0C`, a
state object from the table at `0x0034E9F8`) stays 0 ("normal") while
walking, jumping and attacking.

**Camera.** The field camera is at `0x003A7F80`: a 4x4 view matrix (row
vectors, `v * M`; the world's up is -y), then the eye (`+0x40`), the point
looked at (`+0x50`, the player's head height, 170 units up) and the up
vector (`+0x60`), each x, y, z, 1 (copies at `0x003A80A0` and `0x003511D0`).
A projection precedes it (`0x003A7EF0`: x scale 2.2568, y scale 3.0091 in
Market Street; `0x003A7E90` holds the GS screen centre 2048, 2048). Against
screenshots the visible picture's half-extent is x/z = 1 / (0.47 * scale)
(townsfolk at three depths and the player's feet agree within a few pixels),
so those scales are about twice the visible frustum's. `--mark-actors`
draws the projection of every object into screenshots; the co-op name tags
use it.

**Enemies and battles.** Rooms spawn enemies from their `.ard` (world files
are listed in `000<world>.idx` inside KH2.IMG; `tools/extract_file.py --world
tt ...`): the `btl` script's program for the current battle number spawns
named groups (`Spawn "b_00"`), and a group's enemies appear when the player
walks into its trigger box (`tools/analysis/ard.py enemies|script|group`).
The map jump's LOCATION carries that battle number (+6), so `--warp-test=
F:02:04:1` puts three Dusks (`M_EX990`, objentry `0x13E`) in the Sandlot,
behind a trigger box around the middle of the room. An object's stats are a
`YS::SHEET` at `+0x48C` (HP, max HP, min HP as three ints; the player's is
the party slot at `0x01C6C750`); its vtable `+0x78` is add_hp `(obj, amount,
sheet index, flag)` (`FUN_00187520`: `FUN_001A1910` clamps the sum to the
min/max, `YS::BTLOBJ::dead` at 0); `+0x81C` points at its spawn entry in the
room data, whose serial (`+0x1E`) names it the same way on every machine.

**Control.** The word at `0x01D48DB8` (GoA's "Cntrl") is a mask of
object types whose control is off: `YS::OBJ::ControlOff(type)` /
`ControlOn(type)` (`0x001B9C28` / `0x001B9C58`) set and clear bit `type`,
and every object's "may act" check (vtable `+0x40`, `FUN_001B9C90`) fails
when its objentry type's bit is set, when its `+0x10C` has `0x200000`, or
while a map jump runs. So the usual value 3 in cutscenes means "player and
party frozen" (types 0 and 1); bit 4 would stop every enemy. Co-op leaves
clients' enemy AI on (each player is attacked by their own copies) and
only moves them; `+0x10C |= 0x200000` would make one object a pure puppet.

**Map jump.** `FUN_00104F58(const LOCATION*, int fade, int delay, u8, int
callback)`: LOCATION is 10 bytes (world, room, door u16, map/battle/event u16;
0xFFFF = the save's choice). Unless the byte at `0x003A7B00` is set (then it
only stores the destination at `0x003A7B08` and starts the callback's fade),
it creates `AREA::MapJumpTask` (`0x00104E98`) in the task manager at
`*0x0032B96C`, stores it at `0x0032BB20`, locks the mutex at `0x00351EC8` and
copies the destination to `0x0032BAF0`. The task waits while a menu or
message window is open, then tears the area down (`FUN_00104E18`).
`YS::TEST_MAPJUMP::Exec` (`0x002468E8`, debug) calls it with fade 1 and
callback 0xC.

## Region and text

`REGION::Get` (`0x00105C30`) returns the region picked at boot from
SYSTEM.CNF's executable name (`FUN_00105B58`, table at `0x0033CB00`: 1 jp,
2 us, 3 uk, 4 it, 5 sp, 6 gr, 7 fr, 8 fm); this disc is 8. The language
string at `0x0033CAF4` (`jp` for 1 and 8, `us` for 3) names the `msg/<lang>/`
folder. Text code asks "Japanese?" as `Get() == 1 || Get() == 8`:

- `FUN_001AAA08` (font set-up; FONT table at `0x01C6CC60`, 0x20 bytes each:
  +0x12/+0x13 cell width/height): the Japanese fonts use 18x18 (sys) and
  24x24 (evt) cells, the international ones 18x24 and 24x32. The two images
  hold different glyph sets in different cells, so the US `fontimage.bar`
  only draws right with the international sizes.
- `FUN_001AB820` (glyph width), `Tz::MenuUtil` padding (`0x0028DE08`,
  `0x0028DEF0`), name and message formatting (`0x00247198`, `0x00247580`,
  `0x002A8330`), command menu items (`0x002B5C30`), save slot world names
  (`0x0015ABA8`).
- `FUN_001678F0` (pad read) uses the same test for the Final Mix button
  layout and right-stick camera, so it has to keep the real answer.

`== 8` alone is Final Mix content (`YS::VOICE` at `0x001AFD18`,
`0x00232348`, `worldmap::Libretto`); `FUN_00105870` builds the save
product code from the region (`KH2J`).

Message text: the international encoding has Latin letters as single bytes
(`A`=0x2E, `a`=0x9A); the Japanese one has kana there and puts lower case in
table 0x1C. Icons are command 0x09 + index into the `icon` font (0x14 SELECT,
0x15 START, 0x16 d-pad, 0x1E-0x21 R1 R2 L1 L2, 0x22-0x25 triangle cross
square circle). The PC release's messages use 0xDA and up for its own
button prompts (`tools/egs_extract.py` maps them back).

Area name cards: each room's map (map/jp/<room>.map) holds the card shown on
entry, a picture (BAR type 24, 512x128 4-bit) and its layout (type 25). The
Japanese card already carries the English subtitle ("-THE USUAL SPOT-"). The
PC release's map/us/<room>.bar holds only a 64x32 placeholder picture with a
zeroed layout (the PC draws its cards from the remastered image itself), so
it cannot be swapped in.

## Model and map textures (BAR type 7)

Header (9 ints): 0, colour count, upload count, environment count,
Offset1 (one byte per environment), upload list (0x90 bytes each),
environment list (GsInfo, 0xA0 bytes each), picture data, palette.

- Each upload is a GIF packet (BITBLTBUF at +0x20, TRXREG at +0x40) and a
  DMA REF tag at +0x70 whose address is the data's offset in the section.
  Upload 0 is the palette.
- Environment n is register writes: TEX0 at +0x70, CLAMP_1 at +0x80. It
  samples upload `Offset1[n] + 1`: its TBP0 is that upload's DBP.
- Maps pack many textures into one 4-bit 256x256 atlas. The environments
  that share it differ in CSA (their own 16 colours) and in CLAMP: region
  clamp or region repeat picks each one's rectangle. A map draws with the
  CLUT already loaded, so CBP in TEX0 is often not the palette's.
- The PC release's "-n.dds" images belong to environment n and cover only
  its CLAMP rectangle (at the artists' scale). Their OriginalAssetOffset is
  close to the data's offset but drifts (by 0x60 + n, or worse in big maps),
  so the data is found through the upload list instead.
- The upload list is the palette's transfer followed by TextureInfoCount
  texture transfers (OpenKH: clutTransfer + textureTransfer).
- After the palette (PaletteOffset + ColorCount * 4) comes a footer of
  tagged blocks ("_DMY", "UVSC", "TEXA", ending at "_KN5"). TEXA is a texture
  animation: texture index, bits per pixel, frame count, the frames' place
  (U, V) and size in the texture, and the frames' data. The game sends a
  frame into the texture at its base pointer, offset to (U, V) (Roxas's
  eyes: 96x48 at 0,20 of his head texture, sent 44 rows high). The PC's
  "-n" images count through the environments, with each animation's frame
  strip (a vertical strip of all its frames) placed among them after its
  texture (Donald: -0 body, -1 his face's frames, -2 and -3 the other
  textures); a strip's offset lies in the footer's frame data.

## Disc size and host files

The disc image is 4.3 GB (0x204330 sectors). PS2Recomp gave host files
(loose `cdrom0:` files, file overrides) sectors from 0x100000 up, inside
the disc: reads of the disc's own data past 2 GB (Olympus Coliseum's
files, among others) returned override files and the game crashed on a
null pointer when entering the world with English text on. Host files now
get sectors after the end of the image (patch 0019).

The PC release's `field2d/us/xx0field.2dd` (the field HUD sheets: gauges,
FINISH!, LEVEL UP!) have bigger pictures (a 512x256 gauge sheet where the
disc has 256x256, 30 % more data) and their own layout; with them this
executable corrupts memory in Olympus Coliseum. The overrides leave them
out; the disc's sheets already use English words.

## Floating point: no infinities, NaNs or writable VF0

The EE's FPU and the VUs saturate: a zero divisor gives +-FLT_MAX, an
overflow FLT_MAX, a square root of a negative number sqrt(|x|), and VF0 is
hardwired to (0, 0, 0, 1). Host floats do none of this, and KH2 depends on
all of it after the Seifer cutscene in the Sandlot (event 78):

- `FUN_001f7e90` (a clip test) runs `vsub.xy vf0, vf4, vf11` and reads the
  status flags (`cfc2 vi16`). Writing the result into VF0 broke every later
  `vdiv Q, vf0w, ...` and `vmaddw ... vf0w`: the camera's view matrix
  (`kn::Camera` at `0x003A7F70`, view at +0x10, eye +0x50, target +0x60, up
  +0x70, roll +0x80; built by `0x00107118`) came out all zero, bone matrices
  too, and the 3D went black behind the HUD. Patch 0023 keeps VF0.
- The IK (`0x00124CA8`, called from the model update `0x00129C38`) then
  normalised zero vectors; the host infinities became NaN positions and
  cameras. Patches 0021 (DIV.S/SQRT.S/RSQRT.S) and 0022 (VU0 VADD/VSUB/VMUL)
  saturate like the hardware.

The status/MAC flags of VU0 macro arithmetic are still not computed, so code
like the clip test above reads stale flags (0: nothing is culled). Computing
them (tried as a patch) made the Struggle orbs' glow fill the screen: the
effect meshes (`0x00217D80` and its siblings, `cfc2 vi17` & 0x11 after the
w of `vmaddw`) are projected with a matrix whose w column is ~1e-15 noise, so
some vertices took the "w <= 0" path (`sqc2 vf0`) and others got huge 1/w.
With the flags at 0 those vertices land far off-screen. The real VU probably
gets exact zeros there; until that is understood the flags stay unset. `PS2X_FPU_LOG=1` prints the sites
of divisions by zero with their `$ra`.

Diagnostics used for this: `KH2_PGS_TRACE_FBP=<fbp|999>:<from>:<lines>[:<to>]`
(state changes of draws into a frame buffer, transfers and CLUT loads, from
the GIF stream), `KH2_PGS_VRAM_DUMP=<tick>,...` (paraLLEl-GS VRAM),
`KH2_RAM_DUMP=<frame>,...` (guest RAM and scratchpad), `KH2_PGS_NO_READBACK`.
