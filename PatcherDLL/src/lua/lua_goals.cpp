#include "pch.h"
#include "lua_goals.hpp"
#include "ai/ai_goals.hpp"

// See lua_goals.hpp and ai/ai_goals.hpp.

namespace {

int arg_count(lua_State* L) { return g_lua.gettop ? g_lua.gettop(L) : 0; }

int character_arg(lua_State* L, int idx)
{
   return arg_count(L) >= idx && g_lua.isnumber(L, idx) ? g_lua.tointeger(L, idx) : -1;
}

void push_bool(lua_State* L, bool b)
{
   if (g_lua.pushboolean) g_lua.pushboolean(L, b ? 1 : 0);
   else if (b) g_lua.pushnumber(L, 1.0f);
   else g_lua.pushnil(L);
}

} // namespace

// SetUnitGoal(charIndex, goal): the AI unit goes in `goal` (from AddAIGoal, of
// its side) now, and back in it each time the game would put it in another;
// the goal takes no one else by weight. True if done.
int lua_SetUnitGoal(lua_State* L)
{
   void* const goal = arg_count(L) >= 2 && g_lua.touserdata ? g_lua.touserdata(L, 2) : nullptr;
   push_bool(L, ai_goals_lua::set_unit_goal(character_arg(L, 1), goal));
   return 1;
}

// ClearUnitGoal(charIndex): back to its team's goals by weight. True if it had one.
int lua_ClearUnitGoal(lua_State* L)
{
   push_bool(L, ai_goals_lua::clear_unit_goal(character_arg(L, 1)));
   return 1;
}

// GetUnitGoal(charIndex): the goal the unit is in now, as AddAIGoal gives it;
// nil for none.
int lua_GetUnitGoal(lua_State* L)
{
   void* const goal = ai_goals_lua::unit_goal(character_arg(L, 1));
   if (goal && g_lua.pushlightuserdata) g_lua.pushlightuserdata(L, goal);
   else g_lua.pushnil(L);
   return 1;
}
