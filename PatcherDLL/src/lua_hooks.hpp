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

// lua_isnumber(L, idx) - returns 1 if the value at idx is a number or a string
// convertible to a number, 0 otherwise.
using fn_lua_isnumber = int(__cdecl*)(lua_State* L, int idx);

namespace lua_addrs {
   // --------------------------------------------------------------------------
   // FOR BF2_modtools exe - fill in from Ghidra
   // --------------------------------------------------------------------------
   namespace modtools {
      // LuaHelper::InitState - initializes Lua and registers all standard libs.
      // We hook this: call original first (fully inits Lua), then read gLuaState_Pointer.
      // Ghidra VA: 0x00486660
      constexpr uintptr_t init_state = 0x486660;

      // Pointer to the exe's global lua_State* variable.
      // Ghidra VA: 0x00B35A58  (RVA/imagebase-offset: 0x735A58, imagebase = 0x400000)
      constexpr uintptr_t g_lua_state_ptr = 0xB35A58;

      // lua_pushcclosure(L, fn, n)
      constexpr uintptr_t lua_pushcclosure = 0x7B86A0;

      // lua_pushlstring(L, s, len) - pushes a string of known length
      // NOTE: We previously mislabeled this as lua_setfield. It's actually pushlstring.
      // Confirmed: called as (L, "__index", 7) in Open_IO.
      constexpr uintptr_t lua_pushlstring = 0x7B8580;

      // lua_settable(L, idx) - sets t[k]=v, pops k and v from stack
      // Lua 5.0 has no lua_setfield; use lua_pushlstring+lua_pushcclosure+lua_settable.
      // Confirmed: PushValue in luaL_openlib loop, called after pushcclosure with (L, tableIdx).
      constexpr uintptr_t lua_settable = 0x7B8960;

      // lua_tolstring(L, idx, len)
      constexpr uintptr_t lua_tolstring = 0x7B7B00;

      // lua_pushnumber(L, n)
      constexpr uintptr_t lua_pushnumber = 0x7B8560;

      // lua_tonumber(L, idx)
      // Confirmed: first CALL inside luaL_checknumber (0x7B7BD0). Returns float in ST0.
      // luaL_checknumber calls it, stores result, compares to 0, checks lua_isnumber if 0.
      constexpr uintptr_t lua_tonumber = 0x7B82A0;

      // lua_gettop(L)
      constexpr uintptr_t lua_gettop = 0x7B7E60;

      // lua_pushnil(L)
      // Confirmed: 1 arg (L only), writes LUA_TNIL=0 to L->top, advances top by 8.
      // Sits 0x20 bytes before lua_pushnumber (0x7B8560) as expected from lapi.c layout.
      constexpr uintptr_t lua_pushnil = 0x7B8540;

      // lua_pushboolean(L, b)
      constexpr uintptr_t lua_pushboolean = 0x7B8720;

      // lua_toboolean(L, idx)
      // Confirmed: returns 0 for nil (tag==0) and boolean-false (tag==1, val==0),
      // returns 1 for everything else. No FPU, no error path. 81 XREFs.
      constexpr uintptr_t lua_toboolean = 0x7B82F0;

      // lua_touserdata(L, idx) - returns light userdata pointer or NULL
      // Confirmed: 0x7B8440
      constexpr uintptr_t lua_touserdata = 0x7B8440;

      // lua_isnumber(L, idx)
      // Confirmed: FUN_007b8070 — checks slot type tag == 3 (LUA_TNUMBER),
      // falls back to lua_str2num for string coercion.
      constexpr uintptr_t lua_isnumber = 0x7B8070;
   }

   namespace steam {
      // LuaHelper::InitState equivalent for Steam exe.
      constexpr uintptr_t init_state = 0xDEAD0000;        // TODO: REPLACE

      // Pointer to the exe's global lua_State* variable.
      // FIND: Same method as modtools - XREF writes after lua_open.
      constexpr uintptr_t g_lua_state_ptr = 0xDEAD0000;   // TODO: REPLACE

      // lua_pushcclosure(L, fn, n)
      constexpr uintptr_t lua_pushcclosure = 0xDEAD0002;   // TODO: REPLACE

      // lua_pushlstring(L, s, len)
      constexpr uintptr_t lua_pushlstring = 0xDEAD0005;    // TODO: REPLACE

      // lua_settable(L, idx)
      constexpr uintptr_t lua_settable = 0xDEAD0003;       // TODO: REPLACE

      // lua_tolstring(L, idx, len)
      constexpr uintptr_t lua_tolstring = 0xDEAD0004;      // TODO: REPLACE

      // lua_pushnumber(L, n)
      constexpr uintptr_t lua_pushnumber = 0xDEAD0006;     // TODO: REPLACE

      // lua_tonumber(L, idx)
      constexpr uintptr_t lua_tonumber = 0xDEAD0007;       // TODO: REPLACE

      // lua_gettop(L)
      constexpr uintptr_t lua_gettop = 0xDEAD0008;         // TODO: REPLACE

      // lua_pushnil(L)
      constexpr uintptr_t lua_pushnil = 0xDEAD0009;        // TODO: REPLACE

      // lua_pushboolean(L, b)
      constexpr uintptr_t lua_pushboolean = 0xDEAD000A;    // TODO: REPLACE

      // lua_toboolean(L, idx)
      constexpr uintptr_t lua_toboolean = 0xDEAD000B;      // TODO: REPLACE

      // lua_touserdata(L, idx)
      constexpr uintptr_t lua_touserdata = 0xDEAD000C;     // TODO: REPLACE

      // lua_isnumber(L, idx)
      constexpr uintptr_t lua_isnumber = 0xDEAD000D;       // TODO: REPLACE
   }
}

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
   fn_lua_touserdata   touserdata   = nullptr;
   fn_lua_isnumber     isnumber     = nullptr;

   int tointeger(lua_State* L, int idx) const { return static_cast<int>(tonumber(L, idx)); }
};

// Global API instance - populated by lua_hooks_install()
extern lua_api g_lua;

// The captured lua_State pointer (set when hook fires)
extern lua_State* g_L;

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
