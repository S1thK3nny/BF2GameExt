#include "pch.h"
#include "lua_comm_orders.hpp"
#include "ai/comm_orders.hpp"

// See lua_comm_orders.hpp and ai/comm_orders.hpp.

namespace {

int arg_count(lua_State* L) { return g_lua.gettop ? g_lua.gettop(L) : 0; }

void push_bool(lua_State* L, bool b)
{
   if (g_lua.pushboolean) g_lua.pushboolean(L, b ? 1 : 0);
   else if (b) g_lua.pushnumber(L, 1.0f);
   else g_lua.pushnil(L);
}

} // namespace

// SetCommandAnyFriendly([on]): true or 1, the player orders any friendly with the
// comm keys Attack and Defend; false or 0, its squad alone; with nothing, its
// soldier class's CommandAnyFriendly decides again.
int lua_SetCommandAnyFriendly(lua_State* L)
{
   int on = -1;
   if (arg_count(L) >= 1) {
      if (g_lua.isnumber(L, 1)) {
         const float v = g_lua.tonumber(L, 1);
         on = v < 0.0f ? -1 : v != 0.0f ? 1 : 0;
      } else {
         on = g_lua.toboolean(L, 1) != 0 ? 1 : 0;
      }
   }
   comm_orders_lua::set_command_any(on);
   return 0;
}

// GetCommandAnyFriendly(): whether the player orders any friendly now.
int lua_GetCommandAnyFriendly(lua_State* L)
{
   push_bool(L, comm_orders_lua::command_any());
   return 1;
}
