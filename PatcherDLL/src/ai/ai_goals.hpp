#pragma once

#include <stdint.h>

// =============================================================================
// AI goals: more of them at once, and units with goals of their own.
//
// Stock BF2 holds 20 AI goals at once across all teams
// (AIGoalManager::mGoals[20], and AIUtil::Init sizes AIGoal::sMemoryPool to 20);
// AddAIGoal past that logs "Error too many AI Goals at one time!" and gives
// nothing. A goal is a team's: every AI unit is put in one of its team's goals by
// weight (AssignUnit, ai_goals_core.hpp) and stays there until it respawns or
// changes side (docs/RE/AISystem.md, "Team goals, joined per unit").
//
// [LimitIncreases] AIGoalLimit (64; 20 or less leaves the game stock) raises the
// limit. The manager is one heap object with mGoals last, so the array grows in
// place: Create is replaced (the game's own operator new and constructor, with
// room for kGoalSlots), the pool holds the limit, and every loop over the array
// counts to it (AddGoal, DeleteGoal, ClearAllGoals, FindGoal, Update, the
// debug listing). AssignUnit keeps the weights in a 20-float array on its stack,
// so it is replaced outright with the same rule over the limit.
//
// A unit of its own (Lua SetUnitGoal(charIndex, goal), ClearUnitGoal and
// GetUnitGoal; the goal is what AddAIGoal returns): the unit is put in that goal
// at once and back in it whenever the game reassigns it (each respawn), while the
// goal is its team's; and the goal is kept for the units a script puts in it,
// none joining it by weight. Deleting the goal (DeleteAIGoal, ClearAIGoals)
// frees them to the weighted rule. A new mission starts with none.
//
// READ ON MODTOOLS (Steam and GOG: not yet):
//
//   AIUtil::Init's pool size         0x0058CD3E PUSH 0x14, MemoryPool::Setup(
//                                    &AIGoal::sMemoryPool 0x00B8FA38, 0x14, 0x3C)
//   AIGoalManager::Create            0x005CD5B0: PUSH 0x6C, CALL operator new
//                                    (0x007E34A0), the constructor (0x005CD0E0,
//                                    thiscall, which clears 20 slots) through its
//                                    thunk, the result into mInstance 0x00B8FAA4
//   AddGoal                          0x005CD130, CMP EDI, 0x14 at +0x6C
//   DeleteGoal                       0x005CD4D0, CMP ESI, 0x14 at +0x18
//   ClearAllGoals                    0x005CD550, MOV EDI, 0x14 at +0xA
//   AssignUnit                       0x005CD5E0, thiscall(Character*), RET 4
//   RemoveUnit                       0x005CDA00, the character on the stack, RET 4
//   the debug listing                0x005CDA70: CMP EDX, 0x14 at +0xA0 (its
//                                    unrolled loop) and +0xB7 (none found), MOV
//                                    EBX, 0x14 at +0x126
//   FindGoal                         0x005CDD00, thiscall(Character*), RET 4;
//                                    CMP ESI, 0x14 at +0x32
//   Update                           0x005CDE20, MOV EDI, 0x14 at +0xC4
//   a goal                           its members' count +0x14, weight +0x18,
//                                    enabled +0x1C, team +0x1D; AddUnit at its
//                                    vptr +0x10, thiscall(Character*)
//   a character                      in a goal while +0x20 (mGoalNode._pList) is
//                                    set; mPlayerId +0x130 (AI below 0),
//                                    mTeamNumber +0x134
//   the teams                        Team::sTeams (0x00AD5D64, a pointer to the
//                                    array), a team's mMemberCount +0x38
// =============================================================================

extern int g_aiGoalLimit;

void ai_goals_install(uintptr_t exe_base);
void ai_goals_uninstall();

// A mission script's units with goals of their own (lua/lua_goals.cpp). Each
// says false (or null) where it did nothing: not installed, no such character
// or goal, or a goal of another side.
namespace ai_goals_lua {
bool  available();
bool  set_unit_goal(int character, void* goal);
bool  clear_unit_goal(int character);
void* unit_goal(int character);   // the goal it is in now, or null
}
