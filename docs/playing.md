# Playing

```powershell
tools\win\run.ps1                      # window, GPU renderer
tools\win\run.ps1 --ssaa=4 --widescreen
```

The easiest way is **`Play KH2.bat`** in the project folder: it starts the
game with the project's files, writes the session's output to
`logs\play.log`, and on a first run creates `build\win\kh2recomp.ini` with
4x super-sampling and widescreen. If the game crashes, `crashes\` gets a
report (every thread's stack) and a minidump; please keep those.

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
| `--ssaa=N` | Super-sampling: 1, 2, 4, 8 or 16 samples per pixel. Smoother edges, less shimmer, and more resolution: 4 and 8 give 2x the PS2's 512x448, 16 gives 4x (2048x1792), enough for 1440p and 4K |
| `--no-ssaa-textures` | Let KH2's full-screen effect pass (it redraws every finished frame) drop back to one sample, which brings back the PS2-resolution look |
| `--no-mipmaps` | Always sample full-size textures (menu: Sharpest textures) |
| `--backbuffer-promotion` | paraLLEl-GS hack: show the source of a final scaling blit (experimental) |
| `--window=WxH` | Initial window size (default 1280x960) |
| `--fullscreen` | Start fullscreen. **Alt+Enter** or **F11** toggles |
| `--vsync` | Present in step with the display (default: lowest latency, no tearing) |
| `--no-fsr` | Bilinear scaling to the window instead of AMD FSR 1 with sharpening (FSR is only used when the window is taller than the picture) |
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
it) — handy for dialogue and long walks; during a pre-rendered movie, Tab
skips the movie instead (movies follow their audio and cannot run faster).

**Skipping scenes.** Hold **B** (the PS2's Cross; **X** or **Space** on the
keyboard) for a second to skip the movie or in-engine cutscene that is
playing; cutscenes end the way the Final Mix pause menu's *Skip* ends them.
**Skip movies** and **Skip cutscenes** in the menu (`--skip-movies`,
`--skip-cutscenes`; `skip-movies = 1`, `skip-cutscenes = 1`) skip them all:
with both, a new game reaches Market Street in about half a minute. **F2** toggles the frame-rate display, **F3** the game-state panel (location,
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

Measured on an RTX 3080 (GPU renderer, 1x super-sampling, compiled VU1
programs, real-time pacing), warping into the first room of each world: Twilight
Town, Olympus Coliseum, the Land of Dragons, 100 Acre Wood, the Pride Lands,
Disney Castle, Timeless River, Halloween Town, Port Royal, Space Paranoids and
The World That Never Was hold 60 fps; Hollow Bastion (57, minimum 56), Beast's
Castle (58, minimum 50) and Agrabah (55, minimum 47) dip during their opening
events, which used VU1 programs not recorded before (now recorded).

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
| `no-blur` | No blur effect in some cutscenes |

Enable several with commas: `--patch=widescreen,no-letterbox`. They are built
by `tools/game_patches.py` (run automatically by `tools/win/recomp.ps1`): the
game functions a patch changes are recompiled a second time from a patched copy
of the executable, and the patched versions are swapped in at start-up.

## English text

The disc is Japanese, but the English text of the PC release (KINGDOM HEARTS
HD 1.5+2.5 ReMIX, Steam or Epic) can be used in its place. **`Play KH2.bat`
does all of this by itself** when it finds the PC release (Steam's library
folders and the usual Epic place are searched; `game\pc-release.txt` can
name its `Image\dt` folder) and Python 3 is installed: it extracts the text,
builds the HD texture index (both once) and, on a first run, turns both on.
By hand: extract the text once (the second command adds the menus, layouts
and other pictures with words in them):

```
python tools/egs_extract.py "<game folder>\Image\dt" game\files\english --english
python tools/egs_extract.py "<game folder>\Image\dt" game\files\english --english-images
```

then start with `--english` (or `english = 1` in `kh2recomp.ini`). Dialogue,
menus, tutorials and item names are in English, with the international font;
the PC's button prompts become the PS2 button icons, laid out for this game
(circle confirms, cross jumps). Still Japanese: the area name cards (they live
inside the map files), the small captions on the title menu, and the voices.
The field HUD sheets stay the disc's (the PC's are laid out differently and
crash this executable; the disc's words there are English anyway).

How it works: the files under `game\files\english` replace the game's own
`msg\jp\*`, `menu\jp\*` and so on by name (any folder given with
`--override-dir` works the same way, for mods), and the game's text code is
told it is not running the Japanese version (`port/runtime/english_text.h`).

## HD textures

The PC release also carries high-resolution versions of the game's textures:
menus, the title screen, fonts, message windows, item pictures, tutorials,
characters and places. Build
the index once (`Play KH2.bat` does; it only reads the .pkg files, nothing
is copied):

```
python tools/egs_hd_extract.py "<game folder>\Image\dt" game\files\hd
```

then start with `--hd-textures` (or `hd-textures = 1`; the settings menu has a
switch to compare). Character models and maps are included, with their
palette variants (recoloured enemies, the many textures packed into one
map atlas) and the PC's frames for animated eyes and mouths. Images load in
the background, so a new area shows its PS2 textures for a moment rather
than stalling; at most 1.5 GB of them stay on the GPU (the environment
variable `KH2_HD_BUDGET_MB` changes that). Each texture is recognised by the data the game sends to the
GS, so this works with the English text on or off for the parts both
versions share. Known limits: the PC's button icons are laid out
differently and are left out, and textures the game builds in memory (the
title logo's animation, say) stay as they are. The PC release has to stay
installed where it was when the index was built. When this project's tools
learn to find more, `Play KH2.bat` rebuilds the index (and the game says so
in its log if it was started another way). `--hd-log` lists what matched.

## Co-op (preview)

The network layer is in: positions, rooms and HP travel between up to three
instances, shown in the **F3** panel. In the world, each other player in your
room appears as their own character (tested with Roxas; Sora and his forms
use the same path): a copy the
game itself runs as an NPC, which walks where they walk and plays the
motion they are playing. It takes about a second and a half to appear after
they enter your room (its files load first). The menu can turn puppets off,
or show players by moving an NPC already in the room instead
(`--puppet-mode=borrow`). Each puppet has its player's name over it (Co-op →
**Name tags**, or `--no-name-tags`). Puppets do not fight or take hits yet.

**Shared enemies.** In the host's room the host's game runs the enemies. A
client's copies are puppets of the host's: their own AI is off, and they
stand, turn and move (attack, block, flinch) exactly as the host's do, so an
enemy swings at you where and when it swings for the host, and its attacks
hit whoever is in their way on each machine. A hit a client lands is handled by
that client's game as usual (spark, sound, reaction; the host's motion is
put back 0.6 s later), is sent to the host and counts there, so everyone's hits wear the same enemies down; an
enemy the host's game kills dies for everyone. The hit itself goes to the host
too and is played on the host's enemy without its HP, so it flinches, is
knocked back and reacts there as well, and every other player sees that.
Co-op → **Shared enemies** turns sharing off on a client
(`--no-shared-enemies`); `KH2_ENEMY_AI=1` keeps the client's own enemy AI
running as the first version did.

**Partners: the other players in your party.** Each player keeps their own
game: their own story, inventory, equipment and choices. When someone joins,
they take the place of your first companion (Donald, or whoever the world
puts in that slot); with three players, the one with the lower number takes
the first companion's place and the other the second (Goofy's), in
the way Dark Souls III's seamless co-op puts a friend in your world:

- They look like themselves, with their own weapon. When someone joins (or
  changes look, e.g. a drive form), the room reloads once you are free to
  move and out of battle, and puts you back where you stood.
- While they are in another room, your companion waits out of sight.
- The companion stops acting on its own: it stands, walks and moves where
  that player does in their game, playing their motions.
- The party HUD shows their HP and MP. Healing them works as healing a party
  member does (Cure, items): the HP your game gives them is sent to their
  game. Damage your enemies do to them is ignored; their own game decides
  what hurts them.
- Menus call them by their name: the camp menu, status and ability screens
  and level-up windows show the player's name where Donald's (or Goofy's)
  would be.
- The party menu shows their equipment, items and abilities. You cannot
  change them: an edit is undone, and so is what it did to your inventory.
- Drive forms do not send them away (nor your other companion) while a
  partner is linked: forms cost no party members in co-op.

Without a companion in the room (Roxas' days, solo segments), a partner shows
up as an NPC copy as before. `--partner-test=FRAME` (testing) puts a
stand-in partner that copies you, a second behind, in the first companion's
place without a network.

- **Host**: settings menu (F1) → Co-op → **Host**, or start with `--host`
  (UDP port 27552; `--host=PORT` for another). The game asks your router to
  open that port (UPnP or NAT-PMP, which most home routers have on; it is
  closed again when you stop hosting; `--no-upnp` to not ask), and asks a
  public STUN server how the internet sees you: the menu then shows **the
  address to give the others** (with a Copy button) and what the router said.
  If the router would not open the port, forward UDP 27552 to this PC
  yourself, or let players in one by one (below).
- **Let in** (when someone cannot get in): while joining, a player's Co-op
  menu shows *their* address. They send it to you, you type it under **Let
  in**, and they press **Join** again within 30 seconds: both games send to
  each other at once, which gets through most home routers (hole punching).
  Some mobile and carrier-grade networks map every connection differently
  and cannot be punched through; one of the two needs a forwarded port then.
- `--nat-test` prints what your router supports and how the internet sees
  you, without opening anything.
- **Join**: menu → Co-op → type the host's address → **Join**, or
  `--join=1.2.3.4` (`:PORT` if not the default). `--name=Riku` sets your name.
- Everyone must run the same game build; the host checks.
- Clients can jump to the host's room (**Go to the host's room** in the menu)
  or follow them automatically (**Follow the host**, or `--follow-host`): when
  the host has been in another room for a second and you are free to move,
  the game's own map jump takes you there.

**Shared story** (on by default; Co-op → **Shared story**, `--no-shared-story`).
Any save can join any other: while connected, a client plays in the *host's*
story. The host runs the game; the client's own story is put aside when they
join and comes back when they leave, so their save never moves on in the
host's story:

- The story is the host's: each room's events and battles, the story flags,
  the rooms seen and each world's party. The client keeps their own level,
  HP, items, equipment, abilities, munny and drive forms (and what they earn
  while connected).
- When the host moves to another room, the clients follow (once they are
  free to move); in between, a client may wander off on their own. The
  host's cutscenes and battles bring them back: the host's enemies are there
  for them too.
- Cutscenes are the host's. A cutscene that starts as a room's event begins
  for the client as well (the room loads again with it), and ends for them
  when it ends for the host, watched through or skipped. A cutscene a script
  starts in the middle of an event (talking to someone, say) is not shown to
  the client yet: they get a note, and pick up with whatever follows it.
- Only the host skips. A client holding the skip button asks the host instead
  (the host sees "... asks to skip the cutscene"); the pause menu's Skip is out
  of a client's reach during cutscenes.
- Saving while connected saves the client's own story and their place in it
  (where they were when they joined), with everything they gained meanwhile.
- When the session ends, the client's story comes back and the map jump takes
  them back to where they were when they joined.

Two instances on one PC work too (`--join=127.0.0.1`).

**Local multiplayer (split screen as windows).** Up to three players on one
PC, each in a game window of their own that you can put wherever you like
(side by side, one per monitor, ...):

- Settings menu → **Local players** → **Add player 2** / **Add player 3**, or
  start with `--local-players=2` (or 3). Each opens another copy of the game
  that joins yours (hosting starts by itself if you were not already hosting
  or joined to someone; if you joined someone else's game, the new window
  joins it too). Everything co-op does applies: partners, shared story,
  shared enemies.
- **Controllers:** player N reads the Nth connected controller, whether or
  not their window is the active one (the keyboard goes to the active
  window). Each window's menu can pick another (**This window's
  controller**), or start with `--pad=N`.
- **Saves:** each local player has their own memory card: `mc0` for player
  1, `mc0-p2` and `mc0-p3` next to the game for the others.
- **Windows:** each one remembers where you put it and its size, per player
  (`kh2recomp-windows.ini`). **Arrange windows** tiles them on the current
  monitor: two side by side, three as one large and two small.
- The extra windows start with their sound off (turn it up with their
  menu's Volume), take their settings from player 1 (only player 1's window
  saves settings), and close when player 1's does.
- Running two or three copies of the game needs a strong CPU: each is a
  whole PS2 game.
