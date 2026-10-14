#include "pch.h"
#include "lua_wingmen.hpp"
#include "ai/flyer_wingmen.hpp"
#include "core/pbl_hash.hpp"

#include <cstring>

// See lua_wingmen.hpp and ai/flyer_wingmen.hpp.

namespace {

using flyer_wingmen_lua::WingmanRef;

int arg_count(lua_State* L) { return g_lua.gettop ? g_lua.gettop(L) : 0; }

// Whether argument `idx` was given as anything but nil or false.
bool given(lua_State* L, int idx) { return arg_count(L) >= idx && g_lua.toboolean(L, idx) != 0; }

// A unit argument: an entity pointer, a character index (by its vehicle; with
// `vehicleOnly` false, else its soldier), or a world object's name. A number
// written as a string counts as a character index.
bool unit_arg(lua_State* L, int idx, bool vehicleOnly, WingmanRef& out)
{
   if (arg_count(L) < idx) return false;
   if (g_lua.touserdata)
      if (void* const p = g_lua.touserdata(L, idx)) {
         out.obj = (uint8_t*)p;
         return true;
      }
   if (g_lua.isnumber(L, idx)) {
      out.obj = flyer_wingmen_lua::character_object(g_lua.tointeger(L, idx), vehicleOnly);
      return out.obj != nullptr;
   }
   const char* const name = g_lua.tolstring(L, idx, nullptr);
   if (!name || !*name) return false;
   out.name = pbl_hash(name);
   return out.name != 0;
}

void push_bool(lua_State* L, bool b)
{
   if (g_lua.pushboolean) g_lua.pushboolean(L, b ? 1 : 0);
   else if (b) g_lua.pushnumber(L, 1.0f);
   else g_lua.pushnil(L);
}

void push_string(lua_State* L, const char* s)
{
   if (s) g_lua.pushlstring(L, s, std::strlen(s));
   else g_lua.pushnil(L);
}

void push_character(lua_State* L, int index)
{
   if (index >= 0) g_lua.pushnumber(L, (float)index);
   else g_lua.pushnil(L);
}

} // namespace

// CallWingmen([count]): wingmen are called up now where the player flies, and at
// every takeoff from now on; `count` how many are called up on their own (a
// negative one: the class's own number again). Returns how many fly with the
// player after it.
int lua_CallWingmen(lua_State* L)
{
   const bool setSize = arg_count(L) >= 1 && g_lua.isnumber(L, 1);
   const int size = setSize ? g_lua.tointeger(L, 1) : 0;
   g_lua.pushnumber(L, (float)flyer_wingmen_lua::call(setSize, size));
   return 1;
}

// DismissWingmen(): the flight is let go and its named flyers forgotten; none is
// called up on its own until CallWingmen or the squad key. Returns how many.
int lua_DismissWingmen(lua_State* L)
{
   g_lua.pushnumber(L, (float)flyer_wingmen_lua::dismiss());
   return 1;
}

// AddWingman(unit): the flyer joins the player's flight, now or once it is a
// friendly AI flyer in the air. True once named.
int lua_AddWingman(lua_State* L)
{
   WingmanRef unit;
   push_bool(L, unit_arg(L, 1, true, unit) && flyer_wingmen_lua::add(unit));
   return 1;
}

// RemoveWingman(unit): no longer named, and let go if it flies with the player.
int lua_RemoveWingman(lua_State* L)
{
   WingmanRef unit;
   push_bool(L, unit_arg(L, 1, true, unit) && flyer_wingmen_lua::remove(unit));
   return 1;
}

// SetWingmenOrder(order [, target]): "formup", "engage", "cover" or "attack".
// An attack takes `target`, else the player's own lock or what is under its
// reticule; a target given that names nothing is refused. True if given.
int lua_SetWingmenOrder(lua_State* L)
{
   const char* const order = arg_count(L) >= 1 ? g_lua.tolstring(L, 1, nullptr) : nullptr;
   WingmanRef target;
   const bool hasTarget = given(L, 2);
   if (!order || (hasTarget && !unit_arg(L, 2, false, target))) {
      push_bool(L, false);
      return 1;
   }
   push_bool(L, flyer_wingmen_lua::give_order(order, target, hasTarget));
   return 1;
}

// GetWingmenOrder(): the flight's order, or nil with no flight.
int lua_GetWingmenOrder(lua_State* L)
{
   push_string(L, flyer_wingmen_lua::order());
   return 1;
}

// SetWingmenLocked(locked)
int lua_SetWingmenLocked(lua_State* L)
{
   const bool locked = arg_count(L) >= 1 && (g_lua.isnumber(L, 1) ? g_lua.tonumber(L, 1) != 0.0f
                                                                   : g_lua.toboolean(L, 1) != 0);
   flyer_wingmen_lua::lock(locked);
   return 0;
}

// SetWingmenFormation([name]): a map formation shape by name, or "vee"; with no
// name, the class's own choice. False for a shape the map does not have.
int lua_SetWingmenFormation(lua_State* L)
{
   const char* const name = arg_count(L) >= 1 ? g_lua.tolstring(L, 1, nullptr) : nullptr;
   push_bool(L, flyer_wingmen_lua::formation(name));
   return 1;
}

// SetWingmenLand([land]): true or 1, wingmen land with the player; false or 0,
// they break off; with nothing, the class's WingmanLand and the INI decide again.
int lua_SetWingmenLand(lua_State* L)
{
   int land = -1;
   if (arg_count(L) >= 1) {
      if (g_lua.isnumber(L, 1)) {
         const float v = g_lua.tonumber(L, 1);
         land = v < 0.0f ? -1 : v != 0.0f ? 1 : 0;
      } else {
         land = g_lua.toboolean(L, 1) != 0 ? 1 : 0;
      }
   }
   flyer_wingmen_lua::land(land);
   return 0;
}

// GetWingmenCount()
int lua_GetWingmenCount(lua_State* L)
{
   g_lua.pushnumber(L, (float)flyer_wingmen_lua::count());
   return 1;
}

// GetWingman(i): the i-th wingman (1 first, in slot order): its pilot's
// character index and its flyer; nil past the last.
int lua_GetWingman(lua_State* L)
{
   const int i = arg_count(L) >= 1 && g_lua.isnumber(L, 1) ? g_lua.tointeger(L, 1) : 0;
   uint8_t* const flyer = i >= 1 ? flyer_wingmen_lua::wingman(i - 1) : nullptr;
   if (!flyer) {
      g_lua.pushnil(L);
      return 1;
   }
   push_character(L, flyer_wingmen_lua::pilot(flyer));
   if (g_lua.pushlightuserdata) g_lua.pushlightuserdata(L, flyer);
   else g_lua.pushnil(L);
   return 2;
}

// IsWingman(unit): whether it flies in the player's flight.
int lua_IsWingman(lua_State* L)
{
   WingmanRef unit;
   uint8_t* const flyer = unit_arg(L, 1, true, unit) ? flyer_wingmen_lua::object(unit) : nullptr;
   push_bool(L, flyer && flyer_wingmen_lua::is_wingman(flyer));
   return 1;
}

// GetWingmanLeader(unit): the character leading it, the player's or an AI
// squadron leader's pilot; nil when it flies as no one's wingman.
int lua_GetWingmanLeader(lua_State* L)
{
   WingmanRef unit;
   uint8_t* const flyer = unit_arg(L, 1, true, unit) ? flyer_wingmen_lua::object(unit) : nullptr;
   uint8_t* const leader = flyer ? flyer_wingmen_lua::leader_of(flyer) : nullptr;
   push_character(L, leader ? flyer_wingmen_lua::pilot(leader) : -1);
   return 1;
}

// GetWingmanLostReason(): inside an OnWingmanLost callback, why it left.
int lua_GetWingmanLostReason(lua_State* L)
{
   push_string(L, flyer_wingmen_lua::lost_reason());
   return 1;
}
