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

## Known limits

- **A client in a drive form sees no avatar of a player in their base look
  in a new room until it reverts** (in the room where it drove, the avatar
  stays). A base-look player object made while our player is in a form
  breaks our form player's motion (null calls at 0x0017FE84 at once; tested
  again: `KH2_AVATAR_BASE_IN_FORM=1` makes it anyway, and crashed on the
  first room change). The other way round works: a player in a form is seen
  in its form look by everyone, in every room.

## Tests

- `tools/live/doortime.sh up [F] [B]`, `warm`, `round N [frames]`: a duo on
  the Land of Dragons save; each round prints the timings above.
- `tools/live/doortrio.sh up|warm|round`: the same with a third game.
- `tools/live/valor.sh GAME [again]`, `revert.sh GAME [again]`: a game into
  Valor Form and out (the drive gauge is set first).
- `tools/live/formstress.sh FROM TO`: door rounds, stopping at the first
  null call; with the client in a form, the crash test above.
