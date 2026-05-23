#pragma once

#include "lua_hooks.hpp"

// =============================================================================
// Custom Lua Function Registration
// =============================================================================
// Add your custom Lua-callable C functions here.
// Each function has the signature:  int fn(lua_State* L)
// 
// Return value = number of values pushed onto the Lua stack as results.
//
// Called from hooked_lua_open() after the game's Lua state is initialized.

void register_lua_functions(lua_State* L);

// Detour on stock CreateEntity Lua callback — auto-applies vehicle fixup
// (team + activate) so vehicles spawned via Lua CreateEntity can fire weapons.
void lua_create_entity_hook_install(uintptr_t exe_base);
