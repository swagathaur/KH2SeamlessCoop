# Running and testing from the command line

`kh2recomp.exe` is the native runner. Without options it opens a window and
plays (see [playing.md](playing.md) for controls, renderer and patches). With `--headless` it opens no window and works as a test harness: it
runs a fixed number of guest frames, saves screenshots, feeds scripted
controller input and reports what happened through its exit code. That lets
builds be checked automatically, including by the agent runner.

## Quick start (Windows)

```powershell
tools\win\recomp.ps1          # regenerate generated\ from game\ (after extract_game.py)
tools\win\build.ps1           # build build\win\kh2recomp.exe
tools\win\run.ps1             # play in a window
tools\win\run.ps1 --headless --frames=600 --screenshot-every=60
```

`run.ps1` passes the ELF, the disc image in `Src\` and `--cd-root=game`, plus any
extra arguments.

## Options

| Option | Meaning |
|---|---|
| `--iso=PATH` | Disc image, needed for sector reads of `KH2.IMG` and `OVL.IMG` |
| `--cd-root=DIR` | Directory for loose `cdrom0:` files (default: the ELF's directory) |
| `--headless` | No window. The test options below also work in a window with the GPU renderer |
| `--renderer=gpu\|soft` | GS renderer: `gpu` = paraLLEl-GS on Vulkan (default when windowed), `soft` = the runtime's software rasteriser (default headless). Screenshots come from whichever is active |
| `--frames=N` | Stop after N guest frames (vsyncs) and exit 0 |
| `--timeout=SEC` | Give up after SEC wall-clock seconds, exit 3 |
| `--screenshot-every=N` | Save `shots/frame_XXXXXX.png` every N guest frames |
| `--screenshot-at=A,B,...` | Save screenshots at specific guest frames |
| `--shots=DIR` | Screenshot directory (default `shots`) |
| `--input=SCRIPT` | Scripted controller input (file path or inline text, below) |
| `--status-every=SEC` | Status line interval, 0 to disable (default 2 headless, off windowed) |
| `--sound=native\|off` | Run the disc's sound driver (`LIBSD`, `LIBSSL`) on the emulated IOP, or stub it |
| `--mute` | Keep the sound driver running but play nothing |
| `--trace=ADDR[:N][,ADDR...]` | Log the first 8 calls (`KH2_TRACE_LOG_FIRST=n` for more) to each guest function (args, return address, then `v0`), hex-dumping N bytes at `a0` with `:N`, and add per-function call counts to the status line. Addresses need the `0x` |
| `--gs-dump=A,B,...` | At these guest frames write `gs_XXXXXX.txt` (GS registers, draw contexts, display source, draws grouped by target, then the last 512 GS events in order: GIFtags, register writes, draws, transfers, presents) and `vram_XXXXXX.png` (all 4 MB of GS memory as a 1024×1024 32-bit image) |
| `--skip-movies` | End each movie as soon as it starts streaming (reaches gameplay in ~1 minute instead of ~9) |
| `--speed=X` | Run guest time at X times real time; `0` = unpaced (as fast as the host can). Guest-frame timings (`--input`, screenshots) are unaffected |
| `--profile[=MS]` | Sample every host thread's instruction pointer every MS ms (default 1) and print the hottest functions at exit, overall and for the four busiest threads (Windows; uses the build's PDB) |
| `--dma-log=FRAME,COUNT` | From guest frame FRAME, decode the next COUNT GIF-channel `sceDmaSend` chains: every DMAtag, GIFtag and A+D register write |
| `--missing=stop\|continue` | On a call into untranslated code: stop (default) or keep going |
| `--patch=NAME[,...]`, `--list-patches` | Enable switchable game patches ([playing.md](playing.md)) |
| `--mash=BTN[+BTN]:N` | Tap these buttons every N guest frames: gets through menus and dialogue whatever the load times (frame-numbered `--input` scripts drift when loading takes real time) |
| `--ee-dump=A,B,...` | Print the EE threads (priority, status, wait object, pc/ra/sp), semaphores and event flags at these frames. The snapshot is built lazily: ask for two consecutive frames and read the second |
| `--stack-dump=A,B,...` | Print every host thread's stack at these frames, with symbols; the EE thread's shows the translated guest functions it is in |
| `--watch=ADDR[,ADDR...]` | Print these guest words (hex and as float) on every status line |
| `--mash-until=FRAME` | Stop `--mash` at this frame (so a `--script` can take over) |
| `--script=FILE` | Timed input: lines `START FRAMES BUTTONS\|- [LX LY [RX RY]]` (sticks 0–255, 128 centre, LY 0 = up; `START` may be `+N` after the previous step); see `tools/scripts/walk-test.txt` |
| `--ram-dump=A,B,...` | Write all 32 MB of guest RAM to `SHOTS/ram_FRAME.bin` (for `tools/analysis/find_position.py` and friends) |
| `--actors-at=A,B,...` | Print the live object list (address, objentry id, model, type, position) and the player pointer |
| `--spawn-test=F:ID`, `--preload-test=F:ID[:LOOK]` | Experiments: create objentry ID in front of the player / ask the game to load its files (with LOOK, first dress puppet entry 84C or 84D as entry LOOK, e.g. `84C:5A` for Roxas) |
| `--teleport=F:X,Y,Z` | Put the player at a position at frame F (repeatable) |
| `--skip-cutscenes` | Skip every in-engine cutscene (with `--skip-movies`, free control in Market Street at ~frame 2200) |
| `--menu-at=F,..:S`, `--ff-at=A-B,..` | Windowed runs: open the settings menu at these frames for S seconds; fast-forward over frame ranges |
| `--no-settings` | Ignore kh2recomp.ini (`tools\win\test.ps1` passes it) |
| `--warp-test=F:WW:RR[:B]` | Map jump to world WW room RR (hex) at frame F; B is the room's battle program (which enemy groups spawn; `tools/analysis/ard.py`) |
| `--mark-actors` | Mark every object's projected feet (magenta) and head height (cyan) in screenshots, to check the camera projection |
| `--host`, `--join=IP`, `--name=`, `--net-loss=F`, `--follow-host`, `--puppet-mode=` | Co-op ([playing.md](playing.md)); `--net-loss` drops that fraction of outgoing packets |
| `--watchdog=SEC` | Print every thread's stack when the host loop stalls this long (default 20 s headless) |

`--trace` status entries also list the busiest callers since the last line,
e.g. `00102448:11785(+1167)[00228344*234,...]`. Status lines carry CD read
counts (`cd: reads=N failed=N`).

Environment: `PS2X_GS_PIXSTATS=1` makes the software rasteriser print, every
16M pixels, how many failed the alpha, destination-alpha and depth tests.

Guest frames are counted in vsyncs, so a script replays identically however fast
the host runs.

Frame-triggered options fire when the guest frame passes their frame (the
host loop can see the counter advance by more than one between checks).

Environment switches for debugging: `KH2_PATCH_SKIP_CODE`, `KH2_PATCH_SKIP_DATA`
and `KH2_PATCH_ONLY_FN=addr,...` (game patches, halves or single functions),
`PS2X_GS_KEEP_VRAM=1` (keep the software GS's VRAM copy with the GPU
renderer), `PS2X_AUDIO_DUMP=file` (raw 48 kHz stereo of everything played),
`PS2_VU_PROGRAM_PROFILE=dir` (record VU1 programs, see
[playing.md](playing.md)).

Analysis tools in `tools/analysis/`: `disasm.py` (MIPS disassembly of main
ELF functions with names), `callers.py` (jal/j call sites), `refs.py` (code
referencing an absolute address), `find_position.py` (RAM snapshot diffing).

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Reached `--frames` |
| 1 | Runtime stopped early (call into untranslated code, guest exit, ...) |
| 2 | Bad command line |
| 3 | `--timeout` reached first |
| 4 | Initialisation failed (ELF or runtime) |

## Scripted input

Entries separated by `;` or newlines, `#` starts a comment:

```
<guest-frame> <BUTTON>[+<BUTTON>...] [hold-frames]
```

Buttons: `UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE START SELECT L1 R1 L2 R2 L3 R3`.
Hold defaults to 4 frames. Example, pressing Start at the title screen and
then confirming (the Japanese release confirms with Circle):

```powershell
tools\win\run.ps1 --headless --frames=1500 --screenshot-every=100 --input="900 START;1000 CIRCLE"
```

The input facility is PS2Recomp's `DQ8_PAD_SCRIPT` (from DQ8Recomp's fork);
`--input` sets it. KH2 reads the controller through libpad2, which
`port/runtime/pad2_hle.cpp` serves from the same input.

## Output

- `[status]` lines: wall time, guest frame, resident overlay, overlay switches
- `[kh2-overlay]` lines: each time a different overlay becomes resident
- `[harness]` lines: screenshots, frame target, timeout, final exit code

## Debugging the 3D path

| Tool | Use |
|---|---|
| `PS2X_KERNEL_TRACE_MS=N` (env) | Every N ms, print each guest thread's state, PC, RA and wait object, and semaphores with waiters, to stderr |
| `PS2_VU_CAPTURE_DIR=DIR` (env) | Write VU1 program starts (`.code` microcode, `.data` VU memory, `.state` registers incl. TOP/ITOP) to DIR, for calls 1, 2, 4, 8, … per program |
| `python tools/vudis.py X.code [start] [count]` | Disassemble captured VU microcode (upper/lower pairs; addresses are pair indices) |
| `--gs-dump=F` | The dump covers the last complete frame (up to 32k GS events, patch 0006): draws grouped by target and texture with their total bounding-box area (largest first), then every event in order |

Changing a runtime header that `ps2_runtime.h` includes rebuilds all ~20k
generated objects (they share it through the precompiled header); prefer
changes in `.cpp` files.
