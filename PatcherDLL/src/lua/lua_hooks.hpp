#pragma once

#include "pch.h"

// =============================================================================
// SWBF2 Lua Types
// =============================================================================
// SWBF2 uses Lua 5.0. These types mirror the Lua 5.0 API just enough to call
// the functions we need via their in-exe addresses.

// Opaque Lua state - we only ever pass pointers to it.
struct lua_State;

// The C function signature Lua expects: int fn(lua_State*)
using lua_CFunction = int(__cdecl*)(lua_State*);

// =============================================================================
// Lua function pointer types (matched to in-exe signatures)
// =============================================================================

// lua_pushcclosure(L, fn, n) - pushes a C closure onto the Lua stack
using fn_lua_pushcclosure = void(__cdecl*)(lua_State* L, lua_CFunction fn, int n);

// lua_pushlstring(L, s, len) - push string of known length onto Lua stack
// NOTE: In Lua 5.0, lua_pushstring is a macro over lua_pushlstring.
using fn_lua_pushlstring = void(__cdecl*)(lua_State* L, const char* s, size_t len);

// lua_settable(L, idx) - does t[k]=v where t=stack[idx], k=stack[-2], v=stack[-1], pops both
using fn_lua_settable = void(__cdecl*)(lua_State* L, int idx);

// lua_tolstring(L, idx, len) - get string from Lua stack
using fn_lua_tolstring = const char*(__cdecl*)(lua_State* L, int idx, size_t* len);

// lua_pushnumber(L, n) - push number onto Lua stack
// NOTE: SWBF2 defines lua_Number as float, not double.
using fn_lua_pushnumber = void(__cdecl*)(lua_State* L, float n);

// lua_tonumber(L, idx) - get number from Lua stack
using fn_lua_tonumber = float(__cdecl*)(lua_State* L, int idx);

// lua_gettop (returns number of args on stack)
using fn_lua_gettop = int(__cdecl*)(lua_State* L);

// lua_pushnil
using fn_lua_pushnil = void(__cdecl*)(lua_State* L);

// lua_pushboolean
using fn_lua_pushboolean = void(__cdecl*)(lua_State* L, int b);

// lua_toboolean
using fn_lua_toboolean = int(__cdecl*)(lua_State* L, int idx);

// lua_touserdata(L, idx) - get light userdata pointer from Lua stack
using fn_lua_touserdata = void*(__cdecl*)(lua_State* L, int idx);

// lua_pushlightuserdata(L, p) - pushes a light userdata (raw void*) onto the stack
using fn_lua_pushlightuserdata = void(__cdecl*)(lua_State* L, void* p);

// lua_isnumber(L, idx) - returns 1 if the value at idx is a number or a string
// convertible to a number, 0 otherwise.
using fn_lua_isnumber = int(__cdecl*)(lua_State* L, int idx);

// lua_gettable(L, idx) - pushes t[k] where t=stack[idx], k=stack[-1]; pops k, pushes result
using fn_lua_gettable = void(__cdecl*)(lua_State* L, int idx);

// lua_pcall(L, nargs, nresults, errfunc) - calls function in protected mode
// Returns 0 on success, non-zero on error.
using fn_lua_pcall = int(__cdecl*)(lua_State* L, int nargs, int nresults, int errfunc);

// lua_rawget(L, idx) - does t[k] where t=stack[idx], k=stack[-1]; pops k, pushes result.
// This is 0x7B87C0. Previously mislabeled as lua_rawgeti and used with 3 args (L,idx,n)
// which caused crashes because it only takes 2 args (L, idx).
// We don't currently call this but document it to prevent future confusion.

// lua_rawgeti(L, idx, n) - pushes t[n] where t=stack[idx], raw (no metamethods).
// Confirmed: FUN_007b8810, plain __cdecl(L, t, n).
// Verified from both FUN_007b73e0 (luaL_ref freelist) and FUN_00486a30 call pattern.
using fn_lua_rawgeti = void(__cdecl*)(lua_State* L, int idx, int n);

// lua_settop(L, idx) - sets the stack top (idx >= 0: absolute; idx < 0: relative).
// lua_pop(L, n) is a macro for lua_settop(L, -(n)-1).
// Used to clean up the error string pcall leaves on the stack on failure.
using fn_lua_settop = void(__cdecl*)(lua_State* L, int idx);

// lua_insert(L, idx) - moves the top element into the given position,
// shifting up elements above that position to open space.
// Confirmed: FUN_007b7f20, called as (mState, -2) in thunk_FUN_004869d0
// (the lua_setglobal macro expansion: pushstring, insert(-2), settable).
using fn_lua_insert = void(__cdecl*)(lua_State* L, int idx);

// =============================================================================
// Lua globals - resolved at runtime from exe base + addresses above
// =============================================================================
struct lua_api {
   fn_lua_pushcclosure pushcclosure = nullptr;
   fn_lua_pushlstring  pushlstring  = nullptr;
   fn_lua_settable     settable     = nullptr;
   fn_lua_tolstring    tolstring    = nullptr;
   fn_lua_pushnumber   pushnumber   = nullptr;
   fn_lua_tonumber     tonumber     = nullptr;
   fn_lua_gettop       gettop       = nullptr;
   fn_lua_pushnil      pushnil      = nullptr;
   fn_lua_pushboolean  pushboolean  = nullptr;
   fn_lua_toboolean    toboolean    = nullptr;
   fn_lua_touserdata       touserdata       = nullptr;
   fn_lua_pushlightuserdata pushlightuserdata = nullptr;
   fn_lua_isnumber         isnumber         = nullptr;
   fn_lua_gettable     gettable     = nullptr;
   fn_lua_pcall        pcall        = nullptr;
   fn_lua_rawgeti      rawgeti      = nullptr;
   fn_lua_settop       settop       = nullptr;
   fn_lua_insert       insert       = nullptr;

   int tointeger(lua_State* L, int idx) const { return static_cast<int>(tonumber(L, idx)); }
};

// Global API instance - populated by lua_hooks_install()
extern lua_api g_lua;

// The captured lua_State pointer (set when hook fires)
extern lua_State* g_L;

// Barrel fire origin toggle — when true, WeaponCannon fires from barrel
// hardpoint (mBarrelPoseMatrix) instead of the default aimer position.
extern bool g_useBarrelFireOrigin;

// Modder-configurable load level path used by LoadDisplay::EnterState.
// Defaults to "Load\\load" (vanilla). Set via SetLoadDisplayLevel() before
// the load screen fires (e.g. from ScriptPreInit).
extern char g_loadDisplayPath[260];

// =============================================================================
// OnCharacterExitVehicle — custom callback storage
// =============================================================================

#define CEV_MAX_CBS 64

enum CEVFilterType { CEV_PLAIN = 0, CEV_NAME = 1, CEV_TEAM = 2, CEV_CLASS = 3 };

struct CEVCallback {
   int      regKey;       // Lua globals key (for rawgeti), 0 if empty slot
   int      filterType;   // CEVFilterType
   int      teamFilter;   // for CEV_TEAM
   uint32_t nameHash;     // for CEV_NAME  — PblHash of the instance name string
   void*    classPtr;     // for CEV_CLASS — resolved EntityClass* from registry
};

extern CEVCallback g_cevCallbacks[CEV_MAX_CBS];
extern int g_cevNextKey;

// =============================================================================
// Public interface
// =============================================================================

// Call from install_patches() in dllmain.cpp.
// exe_base = relocated base address of the game exe.
void lua_hooks_install(uintptr_t exe_base);

// Cleanup (optional, called on DLL_PROCESS_DETACH if desired)
void lua_hooks_uninstall();

// Register a single C function as a named Lua global.
void lua_register_func(lua_State* L, const char* name, lua_CFunction fn);