#pragma once

#include <cstdint>

// Health icons that follow the spawned class, its stance and the entered
// vehicle, and the icons of the weapons in use, published from the HUD update
// so they work on multiplayer clients too, where Lua's On* callbacks never fire. Rides on the shared GameEvents
// Open/Update hooks in target_bar_latch.cpp and adds no detour of its own.
// Inert unless a .hud binds one of its events:
//
//   player1.unit.healthTexture              Uint  texture name hash, stance-varied
//   player1.unit.healthTextureDisable       Bool  sent when there is nothing to show
//   player1.unit.stance                     Uint  0 stand, 1 crouch, 2 prone, 3 ball
//   player1.vehicle.healthTexture           Uint  the vehicle or turret seat's icon
//   player1.vehicle.healthTextureDisable    Bool  on foot, or the vehicle has none
//   player1.weaponN.iconTexture             Uint  weapon 1 or 2's IconTexture
//   player1.weaponN.iconTextureDisable      Bool  no weapon there, or it has none
//
// Bind a texture with EventBitmap and EventEnable and its Disable twin with
// EventDisable, as the stock team icons are bound. The icon is the class's stock
// HealthTexture ODF property; <name>_crouch, <name>_prone and <name>_ball are
// shown instead when loaded. The stock vehicle seating mesh is left alone. A
// weapon's icon is its class's stock IconTexture, for whichever weapon the
// stock weapon1/weapon2 events currently describe.
//
// Modtools, Steam and GOG. See docs/user/HUD.md and docs/RE/HUDSystem.md.

// Call only after the shared hooks installed. Never calls engine code itself.
void hud_class_icons_resolve(uintptr_t exe_base);
void hud_class_icons_open();
void hud_class_icons_update();
