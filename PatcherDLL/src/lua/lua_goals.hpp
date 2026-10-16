#pragma once

#include "lua_hooks.hpp"

// =============================================================================
// AI units with goals of their own, from a mission script: the Lua side of
// ai/ai_goals.hpp's ai_goals_lua. Registered with the rest in lua_funcs.cpp;
// docs/user/LUA_API.md, "AI Goals". Modtools only for now, with [LimitIncreases]
// AIGoalLimit above 20: elsewhere each answers as if nothing were done.
//
// A goal is what the game's own AddAIGoal returns; a unit is a character index.
// =============================================================================

int lua_SetUnitGoal(lua_State* L);
int lua_ClearUnitGoal(lua_State* L);
int lua_GetUnitGoal(lua_State* L);
