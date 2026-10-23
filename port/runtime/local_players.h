// Local multiplayer: more players on this PC, each in a game window of their
// own (split screen, as windows you place where you like).
//
// Every local player runs their own copy of the game (a process), joined to
// this one's co-op session over the loopback address, so everything co-op
// does works the same: their own save and story or the host's, partners,
// shared enemies. What makes them local:
//  - each window reads its own controller (--pad=N: the Nth connected one),
//    whether or not it has the keyboard focus;
//  - each player has their own memory card (mc0 for player 1, mc0-p2, mc0-p3
//    next to the game), so their saves stay apart;
//  - the copies started from here close when this one does (a Windows job
//    object), start with their sound at 0 (the menu turns it up), and take
//    their settings from this one;
//  - each window's place and size are remembered per player
//    (kh2recomp-windows.ini), and Arrange windows tiles them on this
//    window's monitor.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kh2local
{
// This copy's local player number (1 = the one you started; --player=N).
void setPlayer(int player);
int player();
// The arguments this copy runs with (settings and command line, without the
// program name), for starting the others.
void setLaunchArgs(const std::vector<std::string> &args, const std::string &exe);

// Start local player `player` (2 or 3): a new window that joins the session
// at `address` (this copy's when it hosts: 127.0.0.1:port) as `name`, reading
// controller `pad` (0-based).
bool spawn(int player, const std::string &address, const std::string &name, int pad, std::string &error);
// Local players started from here that are still running.
std::vector<int> running();

// Window places, per player (x, y, outer width and height).
bool loadPlace(int player, int &x, int &y, int &w, int &h);
void savePlace(int player, int x, int y, int w, int h);

// Tile every game window on the monitor of this one, in player order: two
// side by side, three as one large and two small.
int arrangeWindows();

// The memory card folder for a player (relative to the game's folder).
std::string memoryCardFolder(int player);
} // namespace kh2local
