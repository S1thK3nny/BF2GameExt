#pragma once

#include <stdint.h>

// The player's wingmen on the HUD, for a .hud to draw its own markers:
//
//   player1.wingmen.count             Uint     wingmen in the flight
//   player1.wingmanN.position         Vector3  where wingman N is on screen
//   player1.wingmanN.onScreen         Bool     sent as it comes into view
//   player1.wingmanN.offScreen        Bool     sent as it leaves it, and as the slot empties
//   player1.wingmanN.direction        Vector3  (0, 0, angle) toward it, for EventRotation
//   player1.wingmanN.distance         Float    metres from what the player controls
//   player1.wingmanN.health           Float    its health, 0 to 1
//   player1.wingmanN.disable          Bool     the slot is empty
//   player1.wingmen.target.*          the flight's attack target (attack my target): position,
//                                     onScreen, offScreen, direction, distance, disable
//
// N runs 1..8, the wingmen in slot order. Placed as the command post markers are
// (hud_world_markers_core.hpp: the collision sphere's centre lifted 1.3 m, as the
// stock objective markers sit). Published from the HUD update while a .hud binds
// one of them; while none binds the wingmen's (or the target's), the wingmen get
// the game's own markers instead (flyer_wingmen.cpp, TargetManager). Modtools
// only, as the wingmen are.

void hud_wingmen_resolve(uintptr_t exe_base);
void hud_wingmen_open();
void hud_wingmen_update();

// Whether a .hud binds a wingman's events, or the attack target's.
bool hud_wingmen_bound();
bool hud_wingmen_target_bound();
