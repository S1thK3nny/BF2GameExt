#pragma once

#include "lua_hooks.hpp"
#include <vector>

// Lua 5.0 upvalue pseudo-index
#define lua_upvalueindex(i) (LUA_GLOBALSINDEX - (i))

// Filter flags — combine with |
enum EventFilter : unsigned {
   FILTER_NONE  = 0,
   FILTER_NAME  = 1 << 0,
   FILTER_TEAM  = 1 << 1,
   FILTER_CLASS = 1 << 2,
   FILTER_ALL   = FILTER_NAME | FILTER_TEAM | FILTER_CLASS,
};

struct GameEvent {
   const char*  name;       // e.g. "CharacterFireWeapon"
   unsigned     filters;    // which filters this event supports (EventFilter flags)

   struct Handler {
      int          id;
      int          luaRef;
      unsigned     filterType;   // FILTER_NONE, FILTER_NAME, FILTER_TEAM, or FILTER_CLASS
      int          filterTeam;
      unsigned int filterHash;   // PblHash of filter string (for Name comparison)
      void*        filterClassPtr; // EntityClass* resolved from registry (for Class comparison)
   };

   std::vector<Handler> handlers;
   int nextId = 1;

   // Add a handler, returns handler ID (or -1 on failure)
   int addHandler(lua_State* L, unsigned filterType, int team, const char* str);

   // Remove a handler by ID
   void removeHandler(int id);

   // Dispatch: caller pushes nargs onto the Lua stack first, then calls this.
   // charIndex is used for filter resolution (team/name/class from character array).
   // The args are popped from the stack when dispatch returns.
   void dispatch(int charIndex, int nargs);

   // Register Lua globals: On{name}, On{name}Team, etc.
   // Also clears any stale handlers from a previous Lua state.
   void registerLua(lua_State* L);
};

// Event instances
extern GameEvent g_evtCharacterFireWeapon;
