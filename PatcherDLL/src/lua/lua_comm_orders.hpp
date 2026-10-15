#pragma once

#include "lua_hooks.hpp"

// =============================================================================
// The comm keys' orders from a mission script: the Lua side of
// ai/comm_orders.hpp's comm_orders_lua. Registered with the rest in
// lua_funcs.cpp; docs/user/LUA_API.md, "Comm Key Orders". Modtools only for now:
// elsewhere they do nothing (false).
// =============================================================================

int lua_SetCommandAnyFriendly(lua_State* L);
int lua_GetCommandAnyFriendly(lua_State* L);
