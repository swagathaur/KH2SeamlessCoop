# Gummi missions in co-op: findings and plan

Goal: in a co-op session, the host flies the main Gummi ship and the other
players fly the two Teeny Ships. Every machine sees the same ships (the
host's main ship plus each player's Teeny Ship), the same course and the same
enemies, with the host's game in charge, as in field co-op.

Everything here comes from reading the code (headless Ghidra on the two
overlays, loaded with the main ELF). None of it has been run yet. Addresses
are for SLPM_666.75 and the overlays as loaded at 0x01F18000.

## Findings

### Game modes and how a mission starts

- Game modes are objects; the current mode number is at `0x0032BA24`
  (1 field, 2 Gummi battle, 3 Gummi menu, 4 title; `MENU::SetGameMode`
  0x00103BB0). A mode is entered with `mode->vtbl[0]` = `0x00102080`
  (GAME_MODE::enable).
- Gummi battle mode: `0x0032BA48` (getGumiBattleMode 0x00104190). Its init
  (0x00103F68) loads `ovl_gumibattle.x`, makes a 0xCE0000 heap (`0x0032BA74`),
  an 8-byte pause block (`0x0032BA78`) and calls the overlay's entry.
  Gummi menu mode: `0x0032BA90` (entering it clears the three ship flags).
- What mission: route `0x0032BA7C` (set by the World Map, 0x001041F0),
  level 1-3 `0x0032BA84` and variant `0x0032BA80` (both set by
  0x00104210), EX flag byte `0x0032BA88` (0x00104278). The course file is
  `gumibattle/gm%02d%c.gbx`.
- The menu's sortie (FUN_01F1AE38) copies three blueprints into fixed buffers
  and sets a flag per slot (FUN_01F18A38 -> 0x00104358):
  main ship `0x003A6B00` (0x800) flag `0x0032BAD0`; Teeny 0 `0x003A7300`
  (0x400) flag `0x0032BAD1`; Teeny 1 `0x003A7700` (0x400) flag `0x0032BAD2`.
  It then starts the battle mode (gumimenu 0x01F1B714).
- So a mission can in principle be started with no menu: route, level,
  variant, EX, the buffers and flags, then `0x00102080(0x0032BA48)`.

### Blueprints (gm::PLAN)

- Self-contained: no pointers, only self-relative offsets (+0xD8 blocks,
  +0xDC abilities, +0xE0 texture slots) and ids into the game's own tables.
  The bytes can be copied between machines as they are. Size
  `0xE4 + 4n + align4(n) + 4*abilities` (n = blocks + decogumi); +8/+0xA
  block and decogumi counts, +0xC ability count, +0x90 name.
- The save's garage is at save+0x55A0 (0x00330ED0): 18 main plans of 0x710
  from +0x160, 18 Teeny plans of 0x260 from +0x8080.
- Preset blueprints (the ones missions award, e.g. Highwind Lv7 with
  Wingedge 1 and 2) are records of a BAR the menu loads (FUN_01F7F458):
  32 bytes each, plan ids for main / Teeny 1 / Teeny 2 at +2/+4/+6 and the
  gummi item that grants it at +0xC.
- Teeny Ships are only gated by the menu: the Teeny System item (gummi item
  0xAC, count at `0x0033C990 + id`, save+0x10F0C), slots on the main ship
  (2 minus each ability 0x8F, FUN_01F7EDC8) and cost limits. The mission
  itself makes a Teeny Ship for every slot whose flag is set.
- The mission builds each ship's mesh and loads its block models from the
  plan while loading (FUN_01F2F078 in the load step FUN_01F18290). The plans
  and flags must be in place before the battle mode starts.

### The mission

- Overlay main task (FUN_01F186D0), state in `DAT_01F7DE0C`: 1 load,
  3 set-up (makes the ships, FUN_01F21BE8), 4-5 intro, 6-7 play, then game
  over (0xA), continue (0xC), clear (0xE, FUN_01F3C9D0), retry (0x11),
  back to the World Map (0x12).
- Player ship: 0x1440 bytes, vtable 0x01F81060, `DAT_01F7DE44`; pad at
  +0x4A4 (always PAD(0)), state +0x50C (2 play, 4 dead, 5 continue), flags
  +0x1438, body +0x3E0, Teeny Ships at +0x5D0/+0x5D4, formation controller
  at +0x5E0.
- PADs: `0x0034D6C0 + n*0x60` (0x00167F18(n)); +0 held, +8 pressed (u64),
  +0x30 stick (2 floats). Fire 0x100, formation 0x2000/0x4000, active
  formation 0x800, pause bit 2 of pressed.
- **Teeny Ships have no AI and no controls.** (0x4E0 bytes, vtable
  0x01F810E0, index +0x390, body +0x3A0.) The player ship's formation
  controller (FUN_01F2CA10, 8 formations) works out a target offset for each
  (+0x10/+0x20 Teeny 0, +0x50/+0x60 Teeny 1), smooths toward it
  (FUN_01F280A0 / FUN_01F281A0) and places them from the player's matrix
  (FUN_01F28BF0). Pressing fire fires the main ship and both Teenies
  (FUN_01F27A68), lock-on missiles likewise (FUN_01F27A80/FUN_01F27A98).
  The mission is lost when the main ship and both Teenies are destroyed.
- The course runs on rails: course object 0x01F8F8E0, time at +0xC grows by
  the frame delta, progress +0x10; ships' positions are relative to the rail
  (+0x10..+0x4F).
- Enemies are spawned by an event timeline in the .gbx: an event fires when
  the progress passes it; when a group is all killed or all escaped, the next
  event is picked by that (`event+0x10/+0x12`), so **which enemies come next
  depends on kills**. Enemy: 0x3D0 bytes, vtable 0x01F810B8, param +0x3A0,
  HP +0x3A4 (-1 dead).
- No random numbers in the overlay. What differs between machines is time:
  the frame delta (`0x00349E10`) drives the course, spawn delays, motion and
  some damage, so two games drift apart if one drops frames.
- Results at 0x01F80850 (score, kills, ...); items and treasures collected
  go to 0x01F80408 and reach the save only on a clear (gummi items
  `0x0033C990`, treasures `0x0033CA90`, mission records `0x0033BC10`).
- Pause (GumiPause, `0x00103500(0x33)`): retry, Gummi menu or World Map.

### Co-op today

Nothing in the co-op layer knows about Gummi missions. Clients follow the host
by world and room (`0x0032BAE0`), and the World Map (world 0x0F) is treated
as a room with no party. Expected (not tested): a client stays on the World
Map while the host flies, and their game knows nothing of the mission.
Useful pieces to reuse: game events (blobs, reliable), the host-seeded
randoms (SpawnSeed), the enemy follow (EnemyStates, EnemyHit / EnemyKill)
and the mission state follow (mission.h).

## Design

- **Everyone runs the mission.** Each machine enters the same mission with
  the same three ships. The host's game decides the course time, enemy HP,
  deaths, which event comes next, the results and the end.
- **Ships:** the main ship is the host's (P1). Teeny 0 is P2's, Teeny 1
  P3's. Each player brings **their own Teeny blueprint** (from their garage,
  or a preset if they have none), so every Teeny looks like its pilot's.
  The host gathers the three blueprints and sends them, with the mission
  choice, to everyone before the mission loads. Co-op gives the Teeny
  slots regardless of the Teeny System item.
- **Each machine's pad drives its own ship.** On the host, PAD(0) drives
  the main ship as now. On a client, the main ship becomes a follower of the
  host's (its pad is cut off, its transform comes from the host) and the
  client's pad drives their Teeny Ship through the formation hooks below.
- **Teeny control:** after the formation controller runs, the target
  offset of a player's Teeny is replaced by one moved by that player's stick
  (kept inside the formation's limits), and its fire and lock-on are cut off
  from P1's button and done on its pilot's. A Teeny with no pilot (2-player
  session) keeps the game's formation.
- **Who simulates what:** each pilot's own machine moves their ship (no
  input lag) and sends its rail-relative transform, fire and lock-on; the
  others place that ship there and fire for it. Enemy damage on a client is
  sent to the host as hits (like field EnemyHit); the host applies it and
  sends kills and HP. The host's course time is followed with a gentle
  correction (speed up or slow down a little; jump only if far off).
- **Flow:** the host's menu choices and pause choices (retry, quit) are
  followed by everyone; clients cannot pause the mission themselves. Clear,
  game over and continue follow the host's.
- **Rewards:** each player gets the host's results into their own save on a
  clear (items, treasures, record), or only the host does. *Decision
  needed.*

## Progress

- **Phase 1 works (tested):** `gummip ROUTE LEVEL [EX] [MAIN] [TEENY0] [TEENY1]`
  (live control command) starts a mission straight from a field room with
  the game's preset blueprints, Teeny Ships included, whatever the save has
  unlocked: route 0 level 1 from the Land of Dragons, main ship and both
  Teenies flying. `presets` lists them; `gummi ...` does the same with the
  save's garage (save +0x55A0, valid byte then plans; empty until the player
  builds a ship).
- **Phase 3 works on one machine (tested):** `teenytest K` hands Teeny K to a
  test script. The formation leaves it alone (its target offset is the
  pilot's, kept within 110 x / 70 y of the main ship; the formation's own sit
  60 to each side), it fires on its pilot's button only (the main ship's fire
  and lock-on missiles skip it) and shoots enemies down on its own. The hooks
  are in the Gummi battle overlay's own function table
  (`kh2ovl::kOverlays[..].region()`), patched once at start-up; the main
  ship's update (FUN_01F219A8) is also the frame hook that runs guest jobs
  during a mission. Piloted Teenies have no lock-on missiles yet.
- **Phase 4 works (tested, two games):** when the host's Gummi battle mode
  starts (D_00103F68), the host gives its Teeny Ships to the others (the
  first client Teeny 0, the second Teeny 1; their own blueprint from
  GummiTeeny, else a preset) and sends the mission and the three blueprints
  (GummiSortie, in pieces of 900 bytes). A client starts it itself from
  whatever mode it is in (the job runs from every task's yield, 0x00102448).
  Both games then fly the same main ship and Teeny.
- In a Gummi mode (the menu's or the battle's GAME_MODE state +0x1C is 1)
  the field's co-op stands down. Guest jobs now wait in two queues
  (`kh2guest::kField`, `kGummi`): an avatar queued in the field and run inside
  a mission called the field's allocator (AREA::Alloc) with the field gone.
- The presets are `gumimenu/fm/planinfo.bar`, in the Gummi files' own index
  `000gumi.idx` (KH2.IMG). The game only has that index loaded in the Gummi
  modes (it takes the slot the field keeps its world's index in), so the port
  reads the file from the disc image itself (`kh2cd::readGameFile`). Routes are
  0-8, each with variants a-c (`gm00a.gbx` .. `gm08c.gbx`).

## Plan

Each phase ends with a test on the PC (live harness, two or three games).

1. **Start a mission from a command (one game).** A `gummi ROUTE LEVEL [ex]`
   control command that fills the mission globals, the ship buffers (the
   save's own ships or a preset) and starts the battle mode. Prove it from
   the World Map, then from a field room. Gives a quick way into any mission
   for every later test. *Risk:* the battle mode may expect state the menu
   sets up (heap, fades, BGM); then we start the menu mode and drive it.
2. **Teeny Ships on demand (one game).** Fill the Teeny buffers and flags
   from a preset or a garage plan, whatever the save has unlocked. Check
   both Teenies appear, look right and fight in formation.
3. **A Teeny that flies on its own (one game).** Hooks on the formation
   update (target offsets) and on Teeny fire / lock-on; drive Teeny 0
   from a test input (a second pad or a script). Tune limits, speed and
   aiming. Make sure the main ship still dies and continues correctly.
4. **Same mission, same ships, on every machine.** A GummiSortie event
   (route, level, variant, EX, three blueprints, flags) from the host when it
   launches; clients load it into their buffers and enter the battle mode
   themselves (from wherever they are; they come back where they were).
   Clients send their Teeny blueprint to the host on joining. Checks: every
   machine shows identical ships; retry and quit follow the host.
5. **Ships in step.** A GummiState message each frame or two: course time,
   each ship's rail-relative transform, fire, lock-on, alive. The main ship
   on clients and the remote Teenies everywhere follow it; the local pilot
   keeps control of theirs. Check with three games.
6. **Enemies in step.** Host-led: client kills and event branches turned
   off; hits from clients sent to the host; the host's spawns, HP, deaths
   and next events followed (enemies matched by event + record + slot, the
   same on every machine). Score, results, clear and game over follow the
   host's. Check by killing with each player's ship.
7. **Finish.** Rewards rule, HUD (who flies which Teeny, Teeny HP), 2-player
   sessions (one Teeny piloted), a player joining or leaving mid-mission,
   frame drops on one machine, Android.

Phases 1-3 need only one machine and settle whether the idea works; phase
4 answers the question of identical ships; 5 and 6 are the bulk of the work.

## Open questions

- Whether the battle mode can be entered without the Gummi menu having run.
- What reads `gumimenu/planset.bin` and `tinyset.bin`; the garage header
  (+4..+0x15F); the bits of `0x0033CAE0` beyond fire mode.
- Where blueprint rewards are granted after a clear.
- Whether more than two Teeny slots are possible (the code has exactly two:
  4 players would need another kind of ship).
