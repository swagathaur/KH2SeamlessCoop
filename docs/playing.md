# Playing

```powershell
tools\win\run.ps1                      # window, GPU renderer
tools\win\run.ps1 --ssaa=4 --widescreen
```

Settings can live in `kh2recomp.ini` next to `kh2recomp.exe` (start from
`kh2recomp.ini.example`): keys are the command-line options without `--`, and
command-line arguments override the file. With `elf`, `iso` and `cd-root` set
there, the executable can be started by double-clicking it.

## Renderer

The GS (the PS2's graphics chip) is drawn on the GPU by
[paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), a Vulkan compute
emulation of the GS that keeps the accuracy of a software renderer. It needs a
Vulkan 1.3 GPU (any desktop GPU from the last several years). If Vulkan is not
available the runner falls back to the runtime's software rasteriser, which is
much slower.

| Option | Effect |
|---|---|
| `--ssaa=N` | Super-sampling: 1, 2, 4, 8 or 16 samples per pixel. Sharper edges and less shimmer; 4 is a good start |
| `--window=WxH` | Initial window size (default 1280x960) |
| `--fullscreen` | Start fullscreen. **Alt+Enter** or **F11** toggles |
| `--vsync` | Present in step with the display (default: lowest latency, no tearing) |
| `--no-fsr` | Bilinear scaling to the window instead of AMD FSR 1 with sharpening |
| `--renderer=soft` | Use the software rasteriser |

The picture keeps the game's aspect ratio (4:3, or 16:9 with `--widescreen`),
letterboxed to the window.

## In-game menu

**F1** (or **Select+Start** on a controller) opens the settings menu:
super-sampling, sharp upscaling, fullscreen, VSync, the frame-rate display,
the game patches, button layout and vibration all change immediately, and
**Save settings** writes them to `kh2recomp.ini` next to the executable. The
menu works with the mouse, keyboard or controller; the game is paused for
input while it is open. **F2** toggles the frame-rate display, **Alt+Enter** or
**F11** fullscreen.

## Speed: compiled VU1 programs

KH2 does its 3D transforms in VU1 microprograms. Interpreted, they cap 3D
scenes at roughly 10 fps; compiled to native code they run at full speed. The
programs come from the game's data, so they are recorded locally and compiled
by the build:

```powershell
tools\win\vu-record.ps1        # play a while (new areas add new programs)
tools\win\build.ps1            # compiles everything recorded in build\vu-profile
```

Programs not recorded yet still run, interpreted. Recording more is always safe.

## Controls

Xbox-style controllers through XInput (most others work through Steam Input),
and the keyboard. The bottom face button confirms and attacks, as in the
international releases (KH2 Final Mix itself uses Circle); `--swap-confirm`
gives the Japanese layout. Rumble works on XInput pads.

| PS2 | Controller | Keyboard |
|---|---|---|
| D-pad | D-pad | Arrow keys |
| Left stick | Left stick | W A S D |
| Right stick | Right stick | I J K L |
| Circle | A (bottom) | C, Esc |
| Cross | B (right) | X, Space |
| Square | X | Z |
| Triangle | Y | V |
| L1 / R1 | LB / RB | Q / E |
| L2 / R2 | LT / RT | Left / Right Shift |
| L3 / R3 | Stick clicks | R / F |
| Start / Select | Start / Back | Enter / Tab |

## Game patches

Switchable patches, from PCSX2's patch database (authors credited in
`config/game_patches.toml`). `--list-patches` shows what the build has.

| Patch | Effect |
|---|---|
| `widescreen` (or `--widescreen`) | 16:9 with a wider field of view; UI, fonts and subtitles adjusted; cutscene depth of field off |
| `no-letterbox` | No black bars in cutscenes |
| `no-subtitles` | No cutscene subtitles |
| `no-blur` | No blur effect in some cutscenes |

Enable several with commas: `--patch=widescreen,no-letterbox`. They are built
by `tools/game_patches.py` (run automatically by `tools/win/recomp.ps1`): the
game functions a patch changes are recompiled a second time from a patched copy
of the executable, and the patched versions are swapped in at start-up.
