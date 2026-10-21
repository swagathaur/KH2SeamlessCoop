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
menu works with the mouse, keyboard or controller; the game pauses while it
is open (and, with **Pause when the window is in the background** or
`--pause-unfocused`, while you are in another window).

Hold **Tab** to fast-forward (3x by default; the menu or `--ff-speed=X` sets
it) — handy for dialogue and long walks. **F2** toggles the frame-rate display, **F3** the game-state panel (location,
party, live actors, co-op), **F12** saves a screenshot to `screenshots\`
next to the executable, **Alt+Enter** or **F11** toggles fullscreen.

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

Controllers through SDL3: Xbox, PlayStation (DualShock 4, DualSense), Switch
Pro and most others work directly, with vibration (XInput is the fallback in
builds without SDL), plus the keyboard and mouse. The bottom face button confirms and attacks, as in the
international releases (KH2 Final Mix itself uses Circle); `--swap-confirm`
gives the Japanese layout. The settings menu shows which controller is in use.

| PS2 | Controller | Keyboard |
|---|---|---|
| D-pad | D-pad | Arrow keys |
| Left stick | Left stick | W A S D |
| Right stick | Right stick | I J K L, or the mouse with the right button held |
| Circle | A (bottom) | C, Esc |
| Cross | B (right) | X, Space |
| Square | X | Z |
| Triangle | Y | V |
| L1 / R1 | LB / RB | Q / E |
| L2 / R2 | LT / RT | Left / Right Shift |
| L3 / R3 | Stick clicks | R / F |
| Start / Select | Start / Back | Enter / Backspace |

The mouse camera's speed is in the menu (`--mouse-sensitivity=X`;
`--no-mouse-camera` turns it off). Tab (hold) fast-forwards.

**Rebinding the keyboard.** Settings menu → Controls → **Keyboard**: click a
binding and press a key (Esc cancels), **+** adds a second key, **x** clears
it, **Default keys** resets them all; **Save settings** keeps them. In
`kh2recomp.ini` (or on the command line as `--key-ACTION=...`) they are one
line per changed action, keys separated by commas:

```ini
key-circle = J, MouseLeft
key-cross = K, Space
key-l3 = none
```

Actions: `up` `down` `left` `right` (D-pad), `cross` `circle` `square`
`triangle` `l1` `r1` `l2` `r2` `l3` `r3` `start` `select`,
`lstick-up/down/left/right`, `rstick-up/down/left/right` (camera) and
`fast-forward`. Keys: letters and digits, `F13`-`F24`, `Space` `Enter`
`Escape` `Tab` `Backspace` `Up` `Down` `Left` `Right` `LShift` `RShift`
`LCtrl` `RCtrl` `Insert` `Delete` `Home` `End` `PageUp` `PageDown`
`Numpad0`-`Numpad9`, punctuation by name (`Comma` `Period` `Slash`
`Semicolon` `Quote` `LBracket` `RBracket` `Backslash` `Minus` `Equals`
`Backquote`), mouse buttons (`MouseLeft` `MouseRight` `MouseMiddle`
`Mouse4` `Mouse5`), or a virtual-key code (`0x5A`). F1-F12, Alt and the
Windows keys stay with the host.

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

## Co-op (preview)

The network layer is in: positions, rooms and HP travel between up to three
instances, shown in the **F3** panel. In the world, each other player in your
room is shown by borrowing an NPC that is already there (it walks where they
walk, with its own model for now; the menu can turn this off). Proper
puppets with the right model and animations come next.

- **Host**: settings menu (F1) → Co-op → **Host**, or start with `--host`
  (UDP port 27552; `--host=PORT` for another). Others need to reach that port:
  same network, or forward it on your router.
- **Join**: menu → Co-op → type the host's address → **Join**, or
  `--join=1.2.3.4` (`:PORT` if not the default). `--name=Riku` sets your name.
- Everyone must run the same game build; the host checks.
- Clients can jump to the host's room (**Go to the host's room** in the menu)
  or follow them automatically (**Follow the host**, or `--follow-host`): when
  the host has been in another room for a second and you are free to move,
  the game's own map jump takes you there. Story progress is still each
  player's own, so rooms your save has not reached yet may misbehave.

Two instances on one PC work too (`--join=127.0.0.1`).
