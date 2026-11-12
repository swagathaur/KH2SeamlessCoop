# Co-op stability testing

Tools (all in `tools/live`, driving headless games through ctl files):

- `soak.sh SECONDS "GAMES"`: random play: the host to random rooms in
  twelve worlds (also with `SOAK_MINWAIT=20`: before the last load is
  done), drives and reverts (`ctl drive N`, refused as the game's Drive
  command would), damage (`ctl hurt N`), attacks, the pause menu, room
  changes while someone drives, and with `SOAK_KINDS=9` kills and restarts
  of the other games. It stops at the first null call, thread death, pure
  virtual call, dropped job, hang, a game not answering, or a player apart
  from the host for four steps (`health.sh`).
- `churn.sh kill|start GAME`: one game of a running session killed, or
  started again (it rejoins).
- `wipe.sh HOSTFORM CLIENTFORM`: everyone down, the Game Over, Continue.
- `formtest.sh`, `doortime.sh`, `doortrio.sh`: drive forms and room
  changes (see room-changes.md). `mem.sh`: the games' memory.

## What was run (Nov 13)

About 440 soak steps (most with three players), 10 kills and rejoins
(clients and the host, mid-door, mid-drive, in forms), door spam (a jump
every 5-45 frames, with drives and pause menus between), Game Overs in
base form and in drive forms with Continue and Load Game, two players
down while the third changes rooms, spells cast by everyone through room
changes, a Gummi mission with three players (two in drive forms before
it), the host leaving the session mid-mission, and a save test on a
client playing in the host's story. Memory stayed flat (about 2.9 GB each).

## Found and fixed

- A party wipe at 0 HP set by something other than a hit never reached
  the Game Over (both stood at 0 HP).
- A player loading a room counted as gone for the downed rules (a wrong
  Game Over with someone standing; no protection while others loaded).
- The party places left out for avatars were recorded across save swaps
  (the shared story, a loaded save): the wrong party went into a save.
- A client saving in the host's story wrote the host's party places into
  its own save.

## Not covered

Summons through the command menu (the menu's cursor is hard to drive
blind); summons were tested when they were added.
