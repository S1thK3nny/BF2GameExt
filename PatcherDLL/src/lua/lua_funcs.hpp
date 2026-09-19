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

// GetMissionName() validity tracking. Nothing in the engine clears
// GameLoop::mMissionScript on the way back to the shell, so these detours track
// whether it still describes a live mission and GetMissionName() returns nil when
// it does not.
void script_name_tracker_install(uintptr_t exe_base);
void script_name_tracker_uninstall();

// Called from the EventManager::Init detour (mission start). Only ever marks the
// name valid, never invalid, so it cannot make GetMissionName() nil during a
// mission's own ScriptPreInit/ScriptInit.
void script_name_mark_mission_started();
