# Running and testing from the command line

`kh2recomp.exe` is the native runner. Without options it opens a window and
plays. With `--headless` it opens no window and works as a test harness: it
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
| `--headless` | No window; enables the options below |
| `--frames=N` | Stop after N guest frames (vsyncs) and exit 0 |
| `--timeout=SEC` | Give up after SEC wall-clock seconds, exit 3 |
| `--screenshot-every=N` | Save `shots/frame_XXXXXX.png` every N guest frames |
| `--screenshot-at=A,B,...` | Save screenshots at specific guest frames |
| `--shots=DIR` | Screenshot directory (default `shots`) |
| `--input=SCRIPT` | Scripted controller input (file path or inline text, below) |
| `--status-every=SEC` | Status line interval, 0 to disable (default 2) |
| `--sound=native\|off` | Run the disc's sound driver (`LIBSD`, `LIBSSL`) on the emulated IOP, or stub it |
| `--mute` | Keep the sound driver running but play nothing |
| `--trace=ADDR[:N][,ADDR...]` | Log the first 8 calls to each guest function (args, return address), hex-dumping N bytes at `a0` with `:N`, and add per-function call counts to the status line |
| `--gs-dump=A,B,...` | At these guest frames write `gs_XXXXXX.txt` (GS registers, draw contexts, display source, draws grouped by target, then the last 512 GS events in order: GIFtags, register writes, draws, transfers, presents) and `vram_XXXXXX.png` (all 4 MB of GS memory as a 1024×1024 32-bit image) |
| `--skip-movies` | End each movie as soon as it starts streaming (reaches gameplay in ~1 minute instead of ~9) |
| `--speed=X` | Run guest time at X times real time; `0` = unpaced (as fast as the host can). Guest-frame timings (`--input`, screenshots) are unaffected |
| `--profile[=MS]` | Sample every host thread's instruction pointer every MS ms (default 1) and print the hottest functions at exit, overall and for the four busiest threads (Windows; uses the build's PDB) |
| `--dma-log=FRAME,COUNT` | From guest frame FRAME, decode the next COUNT GIF-channel `sceDmaSend` chains: every DMAtag, GIFtag and A+D register write |
| `--missing=stop\|continue` | On a call into untranslated code: stop (default) or keep going |

Guest frames are counted in vsyncs, so a script replays identically however fast
the host runs.

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
