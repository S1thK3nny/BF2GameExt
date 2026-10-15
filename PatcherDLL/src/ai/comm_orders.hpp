#pragma once

#include <stdint.h>

// =============================================================================
// The comm keys in single player ([AI] CommKeyOrders, a prototype, modtools).
//
// Stock, the eight comm keys (F5 to F12 by default: Spotted, Medic, Repair,
// Ammo, Pickup, Backup, Attack, Defend) make team chat only in a networked game
// (RawControllerInputs::TeamOrderInputProcess runs only online), and no AI acts
// on them even there: the chat's TeamAssistManager::AddRequest puts a marker on
// the one asking and plays a shout, and that is all (docs/RE/AISystem.md, "The
// comm keys' requests and the AI's own aid"). Offline, here, each key is a
// request the AI answers, on foot or in a ground vehicle (in a flyer the wingmen
// have F5, F11 and F12: flyer_wingmen.hpp):
//
//   - Spotted: the AI of the player's side within 150 m of the enemy under the
//     crosshair that can see it aim at it where they stand
//     (UnitController::ForceSetAttackHelperTarget), and it is marked.
//   - Medic, Repair, Ammo: the nearest AI within 300 m that can give that aid
//     comes to the player and gives it.
//   - Backup: the three nearest within 200 m fight round the player for 60 s
//     (a Defend goal on the player's character, not a formation: the user, "they
//     should not strictly follow you like how F4 makes them").
//   - Attack: the four nearest within 250 m go after the enemy under the
//     crosshair for 90 s, or until it is down (a Destroy goal on it); aimed at
//     the ground instead, they go to that place and hold it.
//   - Defend: the three nearest guard the friendly under the crosshair for 90 s
//     (a Defend goal on it); aimed at the ground, they hold that place, if a
//     soldier could stand there within 400 m (the user: "aim at a friendly under
//     the cross hair, OR if nothing and they can reach it then the position you
//     are aiming at").
//   - Pickup: not yet (the user: hovers first).
//
// Each key also gets the stock marker and shout. A unit answers one request at
// a time; those following the player as its squad are left to it. The goals
// are AIGoalManager goals kept for the units sent (ai_goals.hpp, SetUnitGoal's
// mechanism); when the time is up each unit goes back to the weighted rule and
// the goal is deleted. The pure rules are in comm_orders_core.hpp.
// =============================================================================

extern bool g_commKeyOrders;

// Read once a frame from a detour of ControllerManager::Update, the AI's clock
// (modtools 0x005997A0, cdecl(float dt)); the AI squads detour it too when they
// are on, and the two chain.
void comm_orders_install(uintptr_t exe_base);
void comm_orders_uninstall();

// The mission is over (EventManager::Cleanup): its requests go, nothing is
// deleted (the goals go with the mission).
void comm_orders_mission_end();

// A goal was deleted or a team's cleared (ai_goals.cpp, after the game's own
// DeleteGoal or ClearAllGoals): an order whose goal is gone forgets it, before
// the goals' pool hands its memory to a new goal.
void comm_orders_goals_changed();

// For the HUD (render/hud_wingmen.hpp): the target of the Attack (0) or Defend
// (1) request under way, an object (`obj`) or a place (`place`); false for none.
bool comm_orders_hud_target(int which, uint8_t** obj, float place[3]);
