# Room changes in co-op

How players follow the host from room to room, and how the party members
standing in for other players are swapped for their avatars. Measured with
`tools/live/doortime.sh` (two games) and `tools/live/doortrio.sh` (three).

## What happens

1. **Intent, not completion.** The host's map jump (MapJump 0x00104F58,
   hooked in `roomsync.cpp`) is broadcast at once as a `RoomIntent` event
   (world, room, door, map, battle, event). Before, clients followed only
   once the host's next state said it had arrived (about a second later).
2. **Clients go at once.** A client with an intent jumps to it as soon as it
   can, whatever it is doing: in battle (also a forced one), mid-combo. Under
   the pause or camp menu the menu is closed first (Tz::MenuBase::SetExit,
   0x00287DA0, run through `kh2gummi::runInAnyMode`), then it goes. Mid-load
   the intent waits (up to 20 s) and is followed once the load is done: a
   second host jump during a client's load is followed straight after.
3. **No settling wait.** Avatars were made 1.5 s after our player was in
   control, for no reason anyone found. They are made at control now
   (`KH2_AVATAR_SETTLE=seconds` puts a wait back, for tests).
4. **Placeholders hidden from the start.** The party member in another
   player's party slot (Donald, Goofy, a world's own) is hidden and frozen
   from the frame the room makes it, not when the avatar arrives.
5. **Pre-spawned.** A player the host just led here, not here yet by their
   own state, is shown beside us at once (for 8 s, or until their state says
   where they are).

`KH2_FAST_ROOMS=0` turns 1-5 off (the old behaviour, for comparison).
`KH2_TIMING=0` turns off the `[timing]` log lines the scripts read.

## Measurements (Land of Dragons 08-01 <-> 08-02, one PC)

Seconds from the host's map jump until every player sees every other's
avatar (ALL_VISIBLE), 5 rounds each:

| | rounds | average |
|---|---|---|
| before (`KH2_FAST_ROOMS=0`) | 4.01 3.31 3.92 3.18 3.87 | 3.66 |
| now | 1.26 1.21 1.25 1.22 1.27 | 1.24 |
| now, three players | 1.39 1.46 1.42 1.48 1.37 | 1.42 |

- The client's own jump: 1.08 s after the host's before, 0.03-0.05 s now.
- A party member seen where an avatar belongs: about 2.2 s (host) and 0.5 s
  (client) before, 0.01 s now (the frame the room is made).
- What is left is the game's own load (control at about 1.1 s). With three
  games on one PC a client's load sometimes takes 2-2.5 s (disc reads at
  once); on separate PCs that goes away.
- With three players the second avatar comes about 0.1 s after the first:
  each is made by a guest job of a few calls, one after the other.

## Fixed along the way

- **Room changes slowed by a log.** A diagnostic printed every file freed for
  4 s after an avatar's teardown; a room change frees dozens, and the host's
  load took 1.7-2.6 s instead of 1.1 s whenever an avatar wore a drive form.
  Off by default now (`KH2_FREE_LOG=1`).
- **A client in a drive form crashed loading the host's enemies.** The
  enemies the host has and our room did not make are read in first (their
  objentry's read request in the room's group, then CACHE_BUFF::Flush). With
  our player in a drive form those reads took the place of the form's files
  and our player's motion called through nothing (0x0017FE84, then
  0x0016B3A8 and 0x0011F250 every frame; the game stuck). In a form they are
  read into kh2formcache's batches now (RAM past 32 MB), as an avatar's form
  look is. Reproduced at will with the client in Valor and
  `ctl.sh client "preload 0300"` (the room's way); seen in 2 of about 12 jumps in a form in the door
  test before the fix; none in 11 after (the new path was taken once, and
  the enemy was made from the batch).

- **An avatar begun as our own room jump started.** At a session's start
  the party refresh reloads the room; the client was in control for that
  one frame, the avatar's making began, and it was finished in the reloaded
  room, where it was taken for our own player. Its removal cleared the
  game's player pointer, and the client followed the host no more (it
  stood with no player). With no settling wait this window was open. Now
  no avatar is begun within 0.3 s of a map jump being asked for or started
  (`kh2roomsync::lastJumpMs`), and one finished for a room since left goes
  the way avatars go (the game's player pointers kept).
- **The enemy read's form check came too early.** It was decided while
  our player was not there yet (the room loading), when it could not tell
  we were in a form: the room's way was used, and with both players in
  Valor the client crashed again. Enemy reads now wait for our player.

## Tried and left off

- **Keeping a worn form look through the room change**
  (`KH2_FORMCACHE_KEEP=1`). A player in a drive form is seen in the next
  room about 0.65-0.75 s after we have control: the room change lets go of
  the look's files (kh2formcache's batch, 9 files, 245 KB for Valor) and
  they are read from the disc again (the reads take that long whatever the
  flush cadence). Kept out of the room's CACHE_BUFF::Clear(0), the avatar
  came 0.07 s after control, but our own load took 0.4-1.1 s longer: with
  our files placed, the room's settling pass runs with the cache top moved,
  the game's old copy of our player's Keyblade (W_EX010, which the form
  look also has) was gone, and the game read it from the disc again
  (`KH2_FORMCACHE_TRACE=1` shows the passes and that read). Our own control
  matters more than the avatar's look, so it stays off.

## Known limits

- (Fixed since: a player in a drive form now sees the others in their base
  look in every room. That was held back because making a base-look player
  object while in a form "crashed"; the crashes were the enemy reads above,
  and a form look's batch taken for Ready after a room change had let its
  files go (an avatar made from freed files; `kh2formcache::want` now reads
  the look again). Tested with `tools/live/formtest.sh`: Valor, Wisdom,
  Limit, Master, Final and Anti on either side, each with the others, with
  room changes and fights in between, and with three players in two
  different forms. `KH2_AVATAR_BASE_IN_FORM=0` brings back the old wait.)

## Tests

- `tools/live/doortime.sh up [F] [B]`, `warm`, `round N [frames]`: a duo on
  the Land of Dragons save; each round prints the timings above.
- `tools/live/doortrio.sh up|warm|round`: the same with a third game.
- `tools/live/formtest.sh h=N c=N r ...`: the host or client drives into
  form N (1 Valor, 2 Wisdom, 3 Limit, 4 Master, 5 Final, 6 Anti; 0
  reverts, through the ctl `drive N` command), or a room change; who sees
  whom and the null calls after each step.
- `tools/live/valor.sh GAME`, `revert.sh GAME`: the same through the
  command menu (fragile: the menu keeps its cursor).
- `tools/live/formstress.sh FROM TO`: door rounds, stopping at the first
  null call; with the client in a form, the crash test above.
