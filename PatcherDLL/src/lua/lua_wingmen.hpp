#pragma once

#include "lua_hooks.hpp"

// =============================================================================
// The player's flyer wingmen from a mission script: the Lua side of
// ai/flyer_wingmen.hpp's flyer_wingmen_lua. Registered with the rest in
// lua_funcs.cpp; docs/user/LUA_API.md, "Flyer Wingmen". Modtools only for now:
// elsewhere each answers as if there were no flight.
//
// A unit argument is a flyer or other world object as the game's own Lua
// functions take one: an entity pointer (GetEntityPtr, GetCharacterVehicle, an
// event's object), a world object's name (a spawner's flyer carries the
// spawner's), or here also a character index (its vehicle; for an attack's
// target, else its soldier).
// =============================================================================

int lua_CallWingmen(lua_State* L);
int lua_DismissWingmen(lua_State* L);
int lua_AddWingman(lua_State* L);
int lua_RemoveWingman(lua_State* L);
int lua_SetWingmenOrder(lua_State* L);
int lua_GetWingmenOrder(lua_State* L);
int lua_SetWingmenLocked(lua_State* L);
int lua_SetWingmenFormation(lua_State* L);
int lua_SetWingmenLand(lua_State* L);
int lua_GetWingmenCount(lua_State* L);
int lua_GetWingman(lua_State* L);
int lua_IsWingman(lua_State* L);
int lua_GetWingmanLeader(lua_State* L);
int lua_GetWingmanLostReason(lua_State* L);
