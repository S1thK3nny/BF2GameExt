#include "pch.h"
#include "lua_hooks.hpp"
#include "lua_funcs.hpp"

#include <detours.h>

lua_api g_lua = {};
lua_State* g_L = nullptr;

static const uintptr_t unrelocated_base = 0x400000;

static void* resolve(uintptr_t exe_base, uintptr_t unrelocated_addr)
{
   return (void*)((unrelocated_addr - unrelocated_base) + exe_base);
}

using fn_init_state = void(__cdecl*)();
static fn_init_state original_init_state = nullptr;

static void __cdecl hooked_init_state()
{
   original_init_state();

   // Strategy B: read gLuaState_Pointer now that InitState has returned
   lua_State** p_lua_state = (lua_State**)((lua_addrs::modtools::g_lua_state_ptr - 0x400000) +
                                           (uintptr_t)GetModuleHandleW(nullptr));
   g_L = *p_lua_state;

   if (g_L)
      register_lua_functions(g_L);
}

void lua_register_func(lua_State* L, const char* name, lua_CFunction fn)
{
   if (not g_lua.pushlstring or not g_lua.pushcclosure or not g_lua.settable) return;

   // Lua 5.0: push name + closure, then settable into LUA_GLOBALSINDEX
   g_lua.pushlstring(L, name, strlen(name));
   g_lua.pushcclosure(L, fn, 0);
   g_lua.settable(L, -10001);
}

void lua_hooks_install(uintptr_t exe_base)
{
   using namespace lua_addrs::modtools;

   g_lua.pushcclosure = (fn_lua_pushcclosure)resolve(exe_base, lua_pushcclosure);
   g_lua.pushlstring  = (fn_lua_pushlstring) resolve(exe_base, lua_pushlstring);
   g_lua.settable     = (fn_lua_settable)    resolve(exe_base, lua_settable);
   g_lua.tolstring    = (fn_lua_tolstring)   resolve(exe_base, lua_tolstring);
   g_lua.pushnumber   = (fn_lua_pushnumber)  resolve(exe_base, lua_pushnumber);
   g_lua.tonumber     = (fn_lua_tonumber)    resolve(exe_base, lua_tonumber);
   g_lua.gettop       = (fn_lua_gettop)      resolve(exe_base, lua_gettop);
   g_lua.pushnil      = (fn_lua_pushnil)     resolve(exe_base, lua_pushnil);
   g_lua.pushboolean  = (fn_lua_pushboolean) resolve(exe_base, lua_pushboolean);
   g_lua.toboolean    = (fn_lua_toboolean)   resolve(exe_base, lua_toboolean);
   g_lua.touserdata   = (fn_lua_touserdata)  resolve(exe_base, lua_touserdata);
   g_lua.isnumber     = (fn_lua_isnumber)    resolve(exe_base, lua_isnumber);

   original_init_state = (fn_init_state)resolve(exe_base, init_state);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_init_state, hooked_init_state);
   DetourTransactionCommit();
}

void lua_hooks_uninstall()
{
   if (original_init_state) {
      DetourTransactionBegin();
      DetourUpdateThread(GetCurrentThread());
      DetourDetach(&(PVOID&)original_init_state, hooked_init_state);
      DetourTransactionCommit();
   }
}
