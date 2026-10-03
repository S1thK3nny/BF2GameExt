#pragma once

#include <cstdint>

// Command post strip: HUD events for a fixed row of command post icons, one
// set per slot, so a .hud can show every post's owner and capture progress.
//
//   player1.commandPosts.count          Uint   posts in the strip
//   player1.commandPostN.icon           Uint   the owning team's SetTeamIcon texture
//   player1.commandPostN.iconDisable    Bool   sent instead when that team has none loaded
//   player1.commandPostN.color          Color  the owner in the viewer's palette
//   player1.commandPostN.capture        Float  how much of the post its side holds, 0..1
//   player1.commandPostN.captureColor   Color  the team gaining or holding it
//   player1.commandPostN.disable        Bool   the slot is not in use
//
// N runs 1..16. Slots follow HUDIndex, then map order; posts with
// HUDIndexDisplay = 0 (the stock invisible posts and command vehicles) and dead
// post objects are left out. A neutral post is team 0: its icon is whatever the
// mission gives team 0 with SetTeamIcon. Published from the HUD update, so it
// runs on multiplayer clients too; there a post's capture only moves while the
// client simulates it, near the local player, as the engine does.
//
// Inert unless a .hud binds one of the events; no INI setting. Modtools, Steam
// and GOG. Addresses are in game/Battlefront2/Source/CommandPost.h and docs/RE/HUDSystem.md.
void hud_command_posts_resolve(uintptr_t exe_base);
void hud_command_posts_open();
void hud_command_posts_update();
