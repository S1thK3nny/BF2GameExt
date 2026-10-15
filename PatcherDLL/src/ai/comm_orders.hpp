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
// have F5, F11 and F12: flyer_wingmen.hpp). The player's squad is the AI
// following it, the squad key's recruits (EVQ_SC_IsFollowingChr); the user,
// after trying it: "On foot, orders should only apply to current
// followers/squadmates recruited via F4", with the exceptions below.
//
//   - Spotted: every friendly within 150 m of the enemy under the crosshair, the
//     squad's or not, that can see it aims at it where it stands
//     (UnitController::ForceSetAttackHelperTarget); the line says "nearby
//     allies".
//   - Medic, Repair, Ammo: a squadmate within 150 m that can give that aid, else
//     the nearest friendly that can, comes to the player and gives it.
//   - Pickup: the nearest friendly AI driving a hover within 300 m comes to the
//     player (a Follow goal), until the player boards it.
//   - Backup: the three nearest friendlies outside the squad within 150 m fight
//     round the player (a Defend goal on its character, not a formation: the
//     user, "they should not strictly follow you like how F4 makes them").
//   - Those four friendlies' help is brief (the user: "maybe 10-15 max"): 15 s
//     once one of them is within 20 m of the player, 30 s at most to get there.
//   - Attack: the squad goes after the enemy under the crosshair for 90 s, or
//     until it is down (a Destroy goal on it); aimed at the ground instead, it
//     goes to that place and holds it.
//   - Defend: the squad guards the friendly under the crosshair for 90 s (a
//     Defend goal on it); aimed at the ground, it holds that place, if a soldier
//     could stand there within 400 m (the user: "aim at a friendly under the
//     cross hair, OR if nothing and they can reach it then the position you are
//     aiming at").
//   - A player whose soldier class says CommandAnyFriendly = 1 (squad_formation's
//     keys, through ClassParent), or for whom a script said
//     SetCommandAnyFriendly(true), orders any friendly with Attack and Defend:
//     the whole squad and the nearest friendlies until there are four (Attack)
//     or three (Defend). The user: "an ODF param should be instated and when
//     set, means you can issue commands to any friendlies without requiring
//     squadmates".
//
// A squadmate stops following to obey (the order's goal replaces its follow);
// when the order ends it follows the player again (EVT_SC_FollowChr, as the
// squad key recruits), and a new squad order takes the squad over from the last
// one. One killed meanwhile is let go: its next life is not sent back. Each key
// also gets the stock chat line, marker and shout. A unit answers one request at
// a time. The goals are AIGoalManager goals kept for the units sent
// (ai_goals.hpp, SetUnitGoal's mechanism); when an order ends its members are
// told to wait, each goes back to the weighted rule and the goal is deleted. The
// pure rules are in comm_orders_core.hpp.
// =============================================================================

extern bool g_commKeyOrders;

// Read once a frame from a detour of ControllerManager::Update, the AI's clock
// (modtools 0x005997A0, cdecl(float dt)); the AI squads detour it too when they
// are on, and the two chain.
void comm_orders_install(uintptr_t exe_base);
void comm_orders_uninstall();

// The mission is over (EventManager::Cleanup): its requests go, nothing is
// deleted (the goals go with the mission), and what a script said with them.
void comm_orders_mission_end();

// A goal was deleted or a team's cleared (ai_goals.cpp, after the game's own
// DeleteGoal or ClearAllGoals): an order whose goal is gone forgets it, before
// the goals' pool hands its memory to a new goal.
void comm_orders_goals_changed();

// For the HUD (render/hud_wingmen.hpp): the target of the Attack (0) or Defend
// (1) request under way, an object (`obj`) or a place (`place`); false for none.
bool comm_orders_hud_target(int which, uint8_t** obj, float place[3]);

// From a mission script (lua/lua_comm_orders.cpp; docs/user/LUA_API.md, "Comm
// Key Orders"), for the rest of the mission.
namespace comm_orders_lua {
// SetCommandAnyFriendly: 1, the player orders any friendly with Attack and
// Defend; 0, its squad alone; -1, its soldier class's CommandAnyFriendly decides
// again.
void set_command_any(int on);
// Whether the player orders any friendly now.
bool command_any();
} // namespace comm_orders_lua
