#include "pch.h"
#include "game_events.hpp"
#include "game/entity/Factory.hpp"
#include "game/controllable/Controllable.hpp"

// ===========================================================================
// PblHash — game's string hashing function
// ===========================================================================
// PblHash::PblHash: __thiscall, ECX = 8-byte buffer, arg = string.
// Result hash is written to buf[0].

using PblHashCtor_t = void*(__thiscall*)(void* buf, const char* name);
static PblHashCtor_t g_pblHashCtor = nullptr;

static unsigned int pbl_hash_string(const char* str)
{
   if (!g_pblHashCtor) {
      if (!g_game.pbl_hash_ctor) return 0;
      const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
      g_pblHashCtor = (PblHashCtor_t)(g_game.pbl_hash_ctor - 0x400000u + base);
   }
   alignas(4) unsigned int hashBuf[2] = {};
   g_pblHashCtor(hashBuf, str);
   return hashBuf[0];
}

// ===========================================================================
// EntityClass registry lookup
// ===========================================================================
// Walks the global PblList<Factory<Entity,EntityClass,EntityDesc>> linked list.
// Returns the Factory* (EntityClass base) whose mId hash matches, or nullptr.

static void* find_entity_class(unsigned int nameHash)
{
   if (!g_game.entity_class_registry) return nullptr;
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto node = *(game::PblListNode<game::Factory>**)(g_game.entity_class_registry - 0x400000u + base);

   __try {
      for (int guard = 0; guard < 4096; ++guard) {
         game::Factory* entry = node->_pObject;
         if (!entry) break;
         if (entry->mId == nameHash)
            return entry;
         node = reinterpret_cast<game::PblListNode<game::Factory>*>(node->_pNext);
      }
   } __except (EXCEPTION_EXECUTE_HANDLER) {}

   return nullptr;
}

// ===========================================================================
// Character data resolution from charIndex
// ===========================================================================

struct CharacterInfo {
   int          team;
   unsigned int nameHash;      // EntityEx.mId (for Name filter — hash comparison)
   void*        entityClassPtr; // EntityEx.mEntityClass (for Class filter — pointer comparison)
};

static bool resolve_character_info(int charIndex, bool needTeam, bool needName,
                                   bool needClass, CharacterInfo& out)
{
   out.team           = -1;
   out.nameHash       = 0;
   out.entityClassPtr = nullptr;

   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };

   if (!g_game.char_array_base_ptr) return false;
   const uintptr_t arrayBase = *(uintptr_t*)res(g_game.char_array_base_ptr);
   if (!arrayBase) return false;

   auto* chr = reinterpret_cast<game::Character*>(arrayBase + charIndex * sizeof(game::Character));

   if (needTeam) {
      __try {
         out.team = chr->mTeamNumber;
      } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
   }

   if (needName || needClass) {
      __try {
         game::Controllable* unit = chr->mUnit;
         if (unit) {
            auto* entity = game::ControllableToEntity(unit);

            if (needName)
               out.nameHash = entity->mEntityEx.mId;

            if (needClass)
               out.entityClassPtr = entity->mEntityEx.mEntityClass;
         }
      } __except (EXCEPTION_EXECUTE_HANDLER) { /* unavailable */ }
   }

   return true;
}

// ===========================================================================
// GameEvent methods
// ===========================================================================

int GameEvent::addHandler(lua_State* L, unsigned filterType, int team, const char* str)
{
   if (g_lua.type(L, 1) != LUA_TFUNCTION) return -1;

   Handler h;
   h.filterType     = filterType;
   h.filterTeam     = team;
   h.filterHash     = 0;
   h.filterClassPtr = nullptr;

   // Resolve filter data at registration time (like the vanilla game)
   if (filterType == FILTER_NAME && str) {
      h.filterHash = pbl_hash_string(str);
   }
   else if (filterType == FILTER_CLASS && str) {
      unsigned int hash = pbl_hash_string(str);
      h.filterClassPtr = find_entity_class(hash);
      if (!h.filterClassPtr) return -1;  // class not found in registry
   }

   g_lua.pushvalue(L, 1);
   int ref = g_lua.L_ref(L, LUA_REGISTRYINDEX);
   if (ref == LUA_NOREF || ref == LUA_REFNIL) return -1;

   h.id     = nextId++;
   h.luaRef = ref;
   handlers.push_back(h);

   dbg_log_verbose("[%s] handler added id=%d filter=%u\n", name, h.id, filterType);
   return h.id;
}

void GameEvent::removeHandler(int id)
{
   for (auto it = handlers.begin(); it != handlers.end(); ++it) {
      if (it->id == id) {
         g_lua.L_unref(g_L, LUA_REGISTRYINDEX, it->luaRef);
         handlers.erase(it);
         dbg_log_verbose("[%s] handler removed id=%d\n", name, id);
         return;
      }
   }
}

void GameEvent::dispatch(int charIndex, int nargs)
{
   // MP clients don't fire event callbacks — matches vanilla game behavior
   if (isMultiplayerClient()) {
      dbg_log_verbose("[%s] dispatch blocked — MP client\n", name);
      if (g_L && nargs > 0) g_lua.settop(g_L, -(nargs + 1));
      return;
   }

   if (!g_L || handlers.empty()) {
      // Still need to pop the args the caller pushed
      if (g_L && nargs > 0) g_lua.settop(g_L, -(nargs + 1));
      return;
   }

   // Determine what filter data we need
   bool needTeam  = false;
   bool needName  = false;
   bool needClass = false;

   for (const auto& h : handlers) {
      if (h.filterType == FILTER_TEAM)  needTeam  = true;
      if (h.filterType == FILTER_NAME)  needName  = true;
      if (h.filterType == FILTER_CLASS) needClass = true;
   }

   CharacterInfo info = { -1, 0, nullptr };
   bool resolved = true;
   if ((needTeam || needName || needClass) && charIndex >= 0)
      resolved = resolve_character_info(charIndex, needTeam, needName, needClass, info);

   // Copy handlers in case a callback modifies the list (e.g. calls Release)
   auto handlersCopy = handlers;

   // The args sit at absolute stack positions argsBase .. argsBase+nargs-1
   int argsBase = g_lua.gettop(g_L) - nargs + 1;

   for (const auto& h : handlersCopy) {
      bool pass = false;
      switch (h.filterType) {
         case FILTER_NONE:
            pass = true;
            break;
         case FILTER_TEAM:
            pass = resolved && (info.team == h.filterTeam);
            break;
         case FILTER_NAME:
            // Hash comparison — matches the game's approach
            pass = resolved && (info.nameHash != 0) &&
                   (info.nameHash == h.filterHash);
            break;
         case FILTER_CLASS:
            // Pointer comparison — matches the game's approach exactly
            pass = resolved && (info.entityClassPtr != nullptr) &&
                   (info.entityClassPtr == h.filterClassPtr);
            break;
      }
      if (!pass) continue;

      // Push callback from the registry
      g_lua.rawgeti(g_L, LUA_REGISTRYINDEX, h.luaRef);
      if (g_lua.type(g_L, -1) != LUA_TFUNCTION) {
         g_lua.settop(g_L, -2);  // pop non-function (stale ref)
         continue;
      }

      // Copy the event args for this call
      for (int i = 0; i < nargs; ++i)
         g_lua.pushvalue(g_L, argsBase + i);

      if (g_lua.pcall(g_L, nargs, 0, 0) != 0)
         g_lua.settop(g_L, -2);  // pop error message
   }

   // Pop the original args
   if (nargs > 0) g_lua.settop(g_L, argsBase - 1);
}

// ===========================================================================
// Generic Lua handlers (closures with GameEvent* as upvalue)
// ===========================================================================

static GameEvent* event_from_upvalue(lua_State* L)
{
   return (GameEvent*)g_lua.touserdata(L, lua_upvalueindex(1));
}

// On{Name}(callback) -> handler
static int lua_OnEvent(lua_State* L)
{
   GameEvent* evt = event_from_upvalue(L);
   if (!evt) { g_lua.pushnil(L); return 1; }
   int id = evt->addHandler(L, FILTER_NONE, 0, nullptr);
   if (id < 0) { g_lua.pushnil(L); return 1; }
   g_lua.pushnumber(L, (float)id);
   return 1;
}

// On{Name}Team(callback, team) -> handler
static int lua_OnEventTeam(lua_State* L)
{
   GameEvent* evt = event_from_upvalue(L);
   if (!evt) { g_lua.pushnil(L); return 1; }
   int team = g_lua.tointeger(L, 2);
   int id = evt->addHandler(L, FILTER_TEAM, team, nullptr);
   if (id < 0) { g_lua.pushnil(L); return 1; }
   g_lua.pushnumber(L, (float)id);
   return 1;
}

// On{Name}Name(callback, nameStr) -> handler
static int lua_OnEventName(lua_State* L)
{
   GameEvent* evt = event_from_upvalue(L);
   if (!evt) { g_lua.pushnil(L); return 1; }
   const char* name = g_lua.tolstring(L, 2, nullptr);
   if (!name) { g_lua.pushnil(L); return 1; }
   int id = evt->addHandler(L, FILTER_NAME, 0, name);
   if (id < 0) { g_lua.pushnil(L); return 1; }
   g_lua.pushnumber(L, (float)id);
   return 1;
}

// On{Name}Class(callback, classStr) -> handler
static int lua_OnEventClass(lua_State* L)
{
   GameEvent* evt = event_from_upvalue(L);
   if (!evt) { g_lua.pushnil(L); return 1; }
   const char* cls = g_lua.tolstring(L, 2, nullptr);
   if (!cls) { g_lua.pushnil(L); return 1; }
   int id = evt->addHandler(L, FILTER_CLASS, 0, cls);
   if (id < 0) { g_lua.pushnil(L); return 1; }
   g_lua.pushnumber(L, (float)id);
   return 1;
}

// Release{Name}(handler) — removes by ID
static int lua_ReleaseEvent(lua_State* L)
{
   GameEvent* evt = event_from_upvalue(L);
   if (!evt) return 0;
   if (!g_lua.isnumber(L, 1)) return 0;
   int id = g_lua.tointeger(L, 1);
   evt->removeHandler(id);
   return 0;
}

// ===========================================================================
// Registration
// ===========================================================================

// Register a closure with a name built from the event name + format string.
// Uses the same pattern as the vanilla LuaHelper::Register (pushcclosure + settable).
static void register_closure(lua_State* L, GameEvent* evt,
                              lua_CFunction fn, const char* fmt)
{
   char buf[128];
   snprintf(buf, sizeof(buf), fmt, evt->name);

   g_lua.pushlightuserdata(L, evt);
   g_lua.pushcclosure(L, fn, 1);
   g_lua.pushlstring(L, buf, strlen(buf));
   g_lua.insert(L, -2);
   g_lua.settable(L, LUA_GLOBALSINDEX);
}

void GameEvent::registerLua(lua_State* L)
{
   // Clear stale handlers from a previous Lua state (map change)
   handlers.clear();
   nextId = 1;

   // Always register On{name} and Release{name}
   register_closure(L, this, lua_OnEvent,      "On%s");
   register_closure(L, this, lua_ReleaseEvent,  "Release%s");

   // Conditional filter variants
   if (filters & FILTER_TEAM)
      register_closure(L, this, lua_OnEventTeam,  "On%sTeam");
   if (filters & FILTER_NAME)
      register_closure(L, this, lua_OnEventName,  "On%sName");
   if (filters & FILTER_CLASS)
      register_closure(L, this, lua_OnEventClass, "On%sClass");

   dbg_log_verbose("[%s] registered Lua closures (filters=0x%x)\n", name, filters);
}

// ===========================================================================
// Event instances
// ===========================================================================

GameEvent g_evtCharacterFireWeapon = { "CharacterFireWeapon", FILTER_ALL };
GameEvent g_evtCharacterExitVehicle = { "CharacterExitVehicle", FILTER_ALL };
