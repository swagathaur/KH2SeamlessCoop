# Boot log

Each blocker hit on the way to the title screen, how it showed up in the
headless harness, and the fix. Newest last.

## 1. Spinning in `InitSystemCallTableAddress` (crt0 → `_InitSys`)

**Symptom:** 600 vsyncs, no display, no overlay. With `PS2X_KERNEL_TRACE_MS`,
the only thread sat at `0x0030EFE0` forever.

**Cause:** Sony's `_InitSys` installs two kernel-mode helpers with
`SetSyscall` (`kFindAddress` at `0x0030EED0` as syscall 0x83, `kCopy` at
`0x0030EE98` as 0x5A), then calls them to locate the kernel syscall table.
Both sat in a gap of the decomp's function map (leaf functions, no stack
prologue), and the runtime fails a redirected syscall whose handler has no
translated function. The search then never terminates.

**Fix:** `make_function_map.py` seeds every analyzer signature match
(`untracked_stubs`) that falls in an unmapped gap.

## 2. Jump to `0x002F1268` with no translated function

**Symptom:** exit 1 at guest frame 11:
`[guest-branch:missing-target] kind=DirectJump op=J source=0x2f1284 target=0x2f1268`.

**Cause:** an empty `jr $ra` function reached only by a tail jump; no prologue,
so neither Ghidra nor prologue seeding found it. A static check found 79 more
branches like it in the main ELF and ~100 from the overlays into the main ELF.

**Fix:** `make_function_map.py` now iterates to a fixpoint: every J/JAL target
outside known functions becomes a function, a call landing inside a function
splits it, and branches from the overlays into the main ELF count too (they
must land on function starts). Seeded functions keep their branch delay slot.
`fix_overlay_maps.py` does the same within each overlay; `check_function_map.py`
reports what is left (8 tail jumps into shared epilogues, which the recompiler
handles as internal entry points).

## 3. SDK wrappers the analyzer does not recognise (pre-emptive)

Found by reading the boot path rather than by a failed run:

| Wrapper | Address | Why it matters |
|---|---|---|
| `sceSifLoadModule` | `0x0030E770` | KH2 retries a module load forever until it succeeds |
| `sceSifLoadModuleBuffer` | `0x0030E508` | IRX images loaded from memory |
| `sceSifCallRpc` | `0x003094B8` | RPCs should go through the runtime's IOP model (DQ8Recomp does the same) |
| `sceCdSearchFile` | `0x002F6FA0` | Called during startup (`FUN_001AE590`); unbound, the real async CD path waits forever (DQ8Recomp hit this) |

All four are bound by hand in `tools/gen_stubs.py` (`MANUAL_STUBS`).

## 4. Indirect call to `0x001061D0` with no translated function

**Symptom:** exit 1 at guest frame 11:
`[guest-branch:missing-target] kind=IndirectCall op=JALR source=0x1024cc target=0x1061d0`
(the main thread creating its memory allocator).

**Cause:** a leaf virtual method (`kn::MemoryAllocator` allocation) reached only
through a vtable; no direct branch or prologue points at it.

**Fix:** `make_function_map.py` scans data for code pointers (833 found). A
target counts if it starts a stack frame, or if it sits in an unmapped gap
directly after a known function (zero padding allowed). The second condition
and a seeding limit at the end of dense code (`0x00325BCC`, past which the
section is data with ten scattered functions) keep data that merely looks like
code from becoming functions. `fix_overlay_maps.py` applies the same rule to the
overlays.

## 5. Sound driver up; archive lookup fails (`\OVL.IMG;1`)

**Symptom:** the run reached its 1,200-frame target (no crash) but nothing was
drawn and no overlay loaded. The log showed the emulated IOP starting Sony's
`LIBSD` and Square Enix's sound driver ("Square-Enix SoundDriver … by Hitoshi
Ohori", RPC server 17), then `sceCdSearchFile failed: \OVL.IMG;1` repeating
forever with the main thread waiting at `0x001AE5F8`.

**Cause:** the runtime's `sceCdSearchFile` only finds loose files under the CD
root and assigns them pseudo sector numbers from `0x100000`. `KH2.IMG` and
`OVL.IMG` exist only inside the ISO, and `KH2.IMG` itself extends past sector
`0x100000` on this disc, so loose copies would collide with real sectors.

**Fix:** `port/runtime/cd_iso.cpp` indexes the ISO9660 directory at start-up
and replaces `sceCdSearchFile` (`0x002F6FA0`) to return each file's real LSN
and size. Sector reads outside registered loose files already go straight to
the ISO, so the archives are read in place.

Also: `iFlushCache` (syscall -0x68), called by KH2's vsync handler every frame,
is now bound to the runtime's handler instead of logging an unimplemented
syscall each time.

## Build-time note: incremental rebuilds

`ps2_recomp` makes every generated file include headers that declare every
function, so adding one function to the map forced a full ~15-minute rebuild.
`tools/recomp.py` now rewrites each file to declare only the functions it
names (most name none; calls go through the runtime by address), and those
headers are out of the precompiled header.

## 6. Title overlay running, screen black: GIF DMA from scratchpad

**Symptom:** the title overlay was resident and the main loop ran every frame,
but every screenshot was black. `--gs-dump` showed zero draw events; GS memory
held only texture uploads (`sceGsExecLoadImage`). `--trace` showed
`sceDmaSend(0x1000A000, 0x70000000)` (GIF channel, source in scratchpad)
thousands of times per second.

**Cause:** KH2 builds its GIF packets in scratchpad and sends them from there.
The runtime's `toDmaPhys` only recognised the bit-31 SPR form
(`0x8000xxxx`) and masked `0x70000000` down to `0x10000000`, the I/O register
area, so every packet the GS should have drawn was read from the wrong place.
Separately, a chain DMAtag with bit 63 set (payload in SPR) lost that bit when
the tag's ADDR field was masked.

**Fix:** `patches/ps2recomp/0002-dma-scratchpad-source.patch` keeps
`0x70000000`–`0x70003FFF` as scratchpad addresses in `toDmaPhys`, and maps a
tag's SPR bit to a scratchpad address in the chain walker.

Two more things kept the screen black after that, found with the GS dump
(which had been reporting "0 events" only because the GS frontend's history
recorder starts paused; `--gs-dump` now unpauses it):

## 7. Textures uploaded 8x too far into GS memory

**Symptom:** draws reached the GS (logo triangles into an off-screen buffer at
FBP 216, then a strip copy to the display buffer), but sampled empty VRAM.

**Cause:** the runtime's `sceGsSetDefLoadImage`/`sceGsExecLoadImage` read the
`dbp` argument as a 2 KB-unit address and multiplied it by 8. In libgraph it is
already BITBLTBUF.DBP (64-word units). Texture 13824 went to
13824 × 8 mod 16384 = 12288, its CLUT 13568 to 10240.

**Fix:** `port/runtime/gs_image.cpp` replaces both with libgraph's real
96-byte `sceGsLoadImage` packet (GIFtag, BITBLTBUF, TRXPOS, TRXREG, TRXDIR,
IMAGE GIFtag) and sends header plus image as one PATH3 packet.

## 8. Tail jumps bypass replaced functions

**Symptom:** with the fix above the texture arrived but its CLUT never did.

**Cause:** KH2 uploads the CLUT with a tail jump (`j sceGsExecLoadImage`).
The recompiler emits a JAL as a dispatch through the runtime's function table,
where `replaceFunction` takes effect, but a J as a direct C++ call to the
translated function or stub wrapper, which skips any replacement.

**Fix:** `tools/recomp.py` (`forward_replaced`) gives every generated stub
wrapper, and every function listed in `config/hle_functions.txt`, a forwarder
to whatever the function table holds for its address (guarded so a wrapper can
still call the original). Port code that replaces a non-stub function must add
it to that list.

**Result:** the boot logos and the copyright screen render.

## 9. Right half of the screen at half brightness

**Symptom:** logos and the copyright screen rendered, but x ≥ 256 was dimmed.

**Cause:** KH2 runs both CRT circuits on one buffer, blended 50/50 (PMODE
ALP=0x7F), with DISPLAY2 one line above DISPLAY1 as a flicker filter. It writes
DISPLAY2 itself as DISPLAY1's DY − 1. The runtime's `sceGsSetDefDispEnv` stub
built DISPLAY with DY=0 (and MAGH=0, DW in pixels), so DY − 1 borrowed through
the upper fields (DY=2047, MAGH=15, DW=4095): circuit 2 became 256 pixels wide.

**Fix:** libgraph's own `sceGsSetDefDispEnv` runs as guest code (DISPLAY now
DX=636, DY=50, MAGH=4, DW=2559). ps2_recomp stubs any function named in the
runtime's stub list, so `tools/make_config.py` hands it a map with that function
renamed `guest_sceGsSetDefDispEnv` (`KEEP_AS_GUEST`). `sceGsResetGraph` stays a
runtime stub; `port/runtime/gs_env.cpp` wraps it to fill the `sceGsGParam`
that libgraph reads (interlace, NTSC, field mode).

**Open:** the same name matching (including a leading-underscore alias) stubs
newlib's `_malloc_r`, `_free_r`, `_calloc_r`, `malloc_extend_top` and
`__malloc_lock`, although `gen_stubs.py` meant the guest to own the C heap.
Boot works with the runtime's allocator; revisit if allocations fail later.

## Performance note: parallel rasterization

At the title screen the run managed ~9 guest fps. `--profile` showed one host
thread saturated in the GS software rasterizer (texture sampling, VRAM
swizzle reads, per-pixel writes) while the EE thread mostly waited for it.

`patches/ps2recomp/0003-gs-parallel-raster.patch` splits each large
primitive (≥ 8192 pixels) into 8-row chunks shared by a small worker pool
(up to 12 threads; `PS2X_GS_THREADS=N` overrides, 0 disables). Each pixel is
computed exactly as before, so frames match the serial path (checked at the
copyright and title screens; a logo fade differed by one step, from timing).
900 guest frames now take 23 s instead of ~100 s (~39 fps). The remaining cost
is the per-pixel path itself (`std::function` VRAM accessors, float
interpolation, `lroundf`), the obvious next step if needed.

## 10. Scripted input ignored headless

**Symptom:** `--input` presses never reached the title menu.

**Cause:** the runtime merges the script in `publishHostState`, which only its
windowed present loop calls (`ps2PadPollHost` returns early without a window).

**Fix:** the harness pump publishes an idle host state every iteration, which
applies the script. NEW GAME and its option screens now respond.

## 11. Callbacks merged into neighbouring functions

**Symptom:** after confirming NEW GAME, a series of
`[guest-branch:missing-target] kind=IndirectCall` stops at addresses inside
known functions (`0x003216B8`, `0x001AFE18`, `0x001E99F0`).

**Cause:** Ghidra merged small functions reached only through pointers into
the function before them; the runtime can only enter a function at its start.

**Fix:** `branchlib.split_after_returns` splits a function after any
unconditional transfer (`jr $ra`, `j`, `b`) when no branch connects the two
parts (functions with jump tables are left alone). Code-built (`lui`/`addiu`)
callback addresses after such a transfer are taken as starts, and pointer
scanning is repeated each gap-filling round, since newly found functions can
build more of them. Main map 13,267 → 13,716 functions, overlays +306; all maps
still check clean.

**Result:** the new-game flow runs through difficulty, vibration and sound
selection and the confirmation, to the white "KINGDOM HEARTS" fade.

## 12. Spinning on VIF1_STAT after the new-game fade

**Symptom:** after the white "KINGDOM HEARTS" fade the screen stayed black.
The kernel trace (`PS2X_KERNEL_TRACE_MS`) showed the main thread running
without yielding at `0x00134080`, libvu1 code polling `VIF1_STAT` until the
VIF1 FIFO and VU1 are idle.

**Cause:** the generated code read `0x10000000` (TIMER0) there, not
`0x10003C00`. ps2_analyzer's `[mmio]` table records each I/O access's address
from the nearest `lui` only, ignoring the `ori`/`addiu` that completes it, and
ps2_recomp substitutes the table's address for the computed one: 130 of 142
sites said `0x10000000`.

**Fix:** `tools/make_config.py` no longer passes the table. Every load and store
then checks its computed address at run time (the generic `READ32`/`WRITE32`
path routes I/O and scratchpad to the runtime).

## 13. AREA::start ran twice and loaded the map over itself

**Symptom:** a call through a null pointer at `0x00142D5C` while entering
Twilight Town (`map/jp/tt01.map`).

**Cause:** the area start runs as a field task (`FIELD::CreateTask`), and its
callback ends with a tail jump to `0x001024A8`, a two-instruction helper that
clears `task->func` so the task runs once. The decomp's recovered names call
that address `printf`, and ps2_recomp binds any function with a runtime stub's
name to the stub, so the runtime's printf ran instead. The task ran again, the
second map load released the first one's data, and the area entry read nulls.
Found by chaining `--trace=ADDR:N` (which now hex-dumps memory at `a0`).

**Fix:** `make_function_map.py` gives a decomp-recovered name that collides with
a runtime stub name an address suffix (`printf_001024a8`). SDK names found by
ps2_analyzer are unaffected (those are matched to real library code).

## 14. Opening movie: libmpeg in an overlay, and CD read callbacks

**Symptom:** after New Game the `movie` overlay became resident but the screen
stayed black at 60 fps.

**Cause, part 1:** Sony's libmpeg is linked into `ovl_movie` and drives the IPU
directly; the runtime does not emulate the IPU, but it has an ffmpeg-backed
libmpeg written for Dragon Quest VIII. ps2_analyzer run over
`build/overlays/movie.elf` finds the library (`config/analyzer/movie.toml`).

**Fix:** `gen_stubs.py --overlay sceMpeg,sceIpu` (plus `--extra` for the three
small entries it does not name: `sceMpegDelete`, `sceMpegIsEnd`,
`sceMpegIsRefBuffEmpty`) writes `config/overlays/movie.stubs.toml`, which
`make_config.py` puts in the overlay's config. The runtime's libmpeg also wrote
DQ8's libmpeg globals at fixed addresses; `patches/ps2recomp/0004` puts that
behind `setMpegLegacyGuestGlobals`, which KH2 turns off.

**Cause, part 2:** still no frames: the stream feeder never called the demuxer.
KH2's async loader (`0x001AE8D8`) starts `sceCdRead` and waits for the CD
callback registered with `sceCdCallback` to report completion. The runtime's
`sceCdCallback` discarded the function and its `sceCdRead` notified no one.

**Fix:** `port/runtime/cd_callback.cpp` records the callback and, after each
read, runs it as an interrupt-context invocation with `SCECdFuncRead`.

Also fixed on the way: the tail-jump forwarders (entry 8) sent a replacement
that called its original back into itself once (ResetGraph ran twice).
Replacements now call originals through `kh2::callOriginal`
(`port/runtime/hle_call.h`), which the forwarders honour.

**Result:** the full opening movie plays with correct colour.

## 15. Movie never ended

**Symptom:** after the opening movie's last frame the screen stayed white;
the picture thread was parked in `sceMpegGetPicture`.

**Cause:** the runtime's libmpeg learns that a stream has run out from the
`sceCdSt*` streaming API (as Dragon Quest VIII feeds it). KH2's movie player
reads the PSS with plain `sceCdRead`, so nothing ever said "no more data".

**Fix:** `port/runtime/movie_eof.cpp` watches ovl_movie's own state (bytes left
to read, a u64 at `0x01F294E0`; read in flight, a byte at `0x01F294E8`) each
frame and calls the runtime's `notifyMpegCdStreamStart`/`notifyMpegCdStreamEof`.
The harness gained `--skip-movies` (end each movie as soon as it starts) and
`--speed=0` (unpaced) for reaching gameplay quickly.

**Result:** the opening (a 258 MB PSS) plays to the end and the game moves on
to the first in-engine scene, Roxas's sepia dream. That scene renders its
background but the 3D geometry only as scattered points, at ~3 fps: next is
the VIF1/VU1 path, which KH2 uses for all 3D.

## 16. First 3D scene: GIF packets split across transfers, and palettes

**Symptom:** in the dream scene the geometry showed as dotted outlines and
diagonal hatching over a flat sepia fill, at 2–3 fps. VU1 output, replayed
from captures with `tools/vureplay`, was correct.

**Causes and fixes** (all in `patches/ps2recomp/`):

- **GIF packets split across transfers** (0007 for PATH3, 0010 for PATH2).
  The GIF keeps its place in a packet between DMA transfers, but the runtime
  parsed every submission as starting with a GIFtag. KH2 sends `BITBLTBUF`…
  `TRXDIR` and an IMAGE GIFtag in one VIF `DIRECT` and the texels in the next,
  so texel data was read as tags: random register writes and hundreds of
  screen-sized sprites and triangles a frame (16 billion pixels processed by
  frame 4500, most of them garbage). The GIF arbiter now keeps each path's
  unfinished tail and forwards only whole tags; the VIF's own IMAGE
  continuation special case (which only worked for a tag at the start of the
  payload) is turned off.
- **CLUT buffer** (0008). The GS copies a palette into an internal buffer when
  `TEX0`/`TEX2` is written with a `CLD` that asks for it; KH2 re-uploads its
  palette area several times a frame, so reading palettes from VRAM at draw
  time used the wrong ones and the alpha test (`ATST=GREATER 16`) discarded
  most texels. The frontend now issues an ordered palette-load command at
  `TEX0` time and the CPU backend samples from the buffer.
- **Depth test details** (0010): `ZTE=0` behaves as "pass", and depth
  saturates to the Z buffer format.

`PS2X_GS_PIXSTATS=1` (0009) prints, every 16M pixels, how many failed the
alpha, destination-alpha and depth tests, with sample depth values.

**Result:** Roxas's room in Twilight Town renders correctly, and the scene
runs several times faster.

Note on applying patches: CMake used to skip a patch when `git apply
--reverse --check` succeeded, which breaks when a later patch edits nearby
lines (0008 next to 0003's includes). It now records applied patches with
their hashes in `third_party/PS2Recomp/.kh2-applied-patches` (copies in
`.kh2-applied/`), reverses from the first changed one and applies the rest,
so only files those patches touch get new timestamps. Deleting the stamp
resets the fork and applies everything again (full rebuild).
