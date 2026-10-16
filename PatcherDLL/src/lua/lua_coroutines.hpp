#pragma once

#include <stdint.h>

// Coroutines that can call the game's own Lua functions. Every stock binding
// reads its arguments from, and pushes its results onto, LuaHelper::mState,
// the one global lua_State, never the state it is called with. A coroutine
// runs on a state of its own, so before this a stock call inside one read the
// main stack's values and left its results there. lua_resume now points
// LuaHelper::mState at the coroutine for as long as it runs. See
// docs/user/LUA_API.md#coroutines and docs/RE/lua_api_bindings.md.
void lua_coroutines_install(uintptr_t exe_base);
void lua_coroutines_uninstall();
