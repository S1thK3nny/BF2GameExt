# SWBF2 Lua 5.0 API Bindings

## Overview

The full Lua 5.0 C API (112 functions) is resolved from the game executable and callable from our injected DLL. This gives our hooks the same power over the Lua VM that the original developers had when building the game.

- **76 `lua_*`** core API functions (stack, types, push/get/set, calls, GC, debug)
- **30 `luaL_*`** auxiliary functions (argument validation, registry refs, metatables, buffers)
- **6 `luaopen_*`** library openers (base, table, io, string, math, debug)

## What This Enables

**Custom event callbacks** — Lua scripts can register functions that our C++ hooks call back in real time. A modder writes `OnWeaponFire(function(shooter, weapon) ... end)` and our DLL detects the fire in C++, then invokes their script with the details.

**Runtime code execution** — Execute Lua code on the fly from C++ with `g_lua.dostring()`. Enables debug consoles, hot-reloading scripts, and dynamic injection.

**Rich data exchange** — Create Lua tables from C++ to pass structured data (weapon info, player stats, event details) instead of loose values.

**Function libraries** — Register entire groups of custom functions under a namespace (e.g. `GameExt.Fire()`, `GameExt.GetWeapon()`) in a single call.

**Debug tooling** — Attach hooks to the Lua VM for profiling, breakpoints, and runtime inspection of local variables and call stacks.

## Architecture

All functions are resolved at DLL injection time from known in-exe addresses (modtools build). The `g_lua` global struct provides typed access to every function.

### Files

| File | Role |
|------|------|
| `lua_hooks.hpp` | Function pointer types, addresses, `lua_api` struct, Lua 5.0 constants |
| `lua_hooks.cpp` | Address resolution, InitState hook, WeaponCannon vtable patch |
| `lua_funcs.hpp` | Custom function registration interface |
| `lua_funcs.cpp` | Custom Lua-callable C functions and registration table |

### Usage

All Lua API functions are accessed through `g_lua`:

```cpp
g_lua.pushstring(g_L, "hello");       // push a string
g_lua.pcall(g_L, 1, 0, 0);           // protected call
g_lua.L_ref(g_L, LUA_REGISTRYINDEX); // store a reference
g_lua.dostring(g_L, "print('hi')");  // execute Lua code
```

### Constants

```cpp
LUA_REGISTRYINDEX  // -10000 — pseudo-index for the registry table
LUA_GLOBALSINDEX   // -10001 — pseudo-index for the globals table
LUA_TNIL           // 0 — type tags for lua_type() return values
LUA_TBOOLEAN       // 1
LUA_TNUMBER        // 3
LUA_TSTRING        // 4
LUA_TTABLE         // 5
LUA_TFUNCTION      // 6
LUA_NOREF          // -2 — luaL_ref sentinel values
LUA_REFNIL         // -1
```

### Field Naming Convention

| Lua function | Struct field | Example |
|---|---|---|
| `lua_pushstring` | `g_lua.pushstring` | `g_lua.pushstring(L, "hello")` |
| `lua_typename` | `g_lua.type_name` | `g_lua.type_name(L, LUA_TSTRING)` |
| `lua_strlen` | `g_lua.str_len` | `g_lua.str_len(L, -1)` |
| `luaL_ref` | `g_lua.L_ref` | `g_lua.L_ref(L, LUA_REGISTRYINDEX)` |
| `luaL_error` | `g_lua.L_error` | `g_lua.L_error(L, "bad arg")` |
| `luaopen_math` | `g_lua.open_math` | `g_lua.open_math(L)` |

Note: `g_lua.tolstring` is kept for backward compatibility — it actually calls `luaL_checklstring`. Use `g_lua.tostring` for the real `lua_tostring`.

### SWBF2-Specific Notes

- `lua_Number` is `float`, not `double` (SWBF2's Lua build configuration)
- Lua version is **5.0.2** (confirmed via `_VERSION` string in binary)
- Steam exe addresses are stubbed (`0xDEAD****`) — modtools only for now
- The engine loads all 6 standard libraries during `InitState`

## Function Reference

### Addresses (modtools)

All addresses are unrelocated (imagebase `0x400000`). Resolution handles ASLR automatically.

#### lua_ Core API

| Function | Address | Category |
|----------|---------|----------|
| `lua_open` | `0x7BDF50` | State |
| `lua_close` | `0x7BDFE0` | State |
| `lua_newthread` | `0x7B7E20` | State |
| `lua_atpanic` | `0x7B7E00` | State |
| `lua_gettop` | `0x7B7E60` | Stack |
| `lua_settop` | `0x7B7E70` | Stack |
| `lua_pushvalue` | `0x7B7FB0` | Stack |
| `lua_remove` | `0x7B7ED0` | Stack |
| `lua_insert` | `0x7B7F20` | Stack |
| `lua_replace` | `0x7B7F70` | Stack |
| `lua_checkstack` | `0x7B7D50` | Stack |
| `lua_xmove` | `0x7B7DB0` | Stack |
| `lua_type` | `0x7B7FE0` | Type checking |
| `lua_typename` | `0x7B8010` | Type checking |
| `lua_isnumber` | `0x7B8070` | Type checking |
| `lua_isstring` | `0x7B80C0` | Type checking |
| `lua_iscfunction` | `0x7B8030` | Type checking |
| `lua_isuserdata` | `0x7B8100` | Type checking |
| `lua_equal` | `0x7B81B0` | Type checking |
| `lua_rawequal` | `0x7B8140` | Type checking |
| `lua_lessthan` | `0x7B8230` | Type checking |
| `lua_tonumber` | `0x7B82A0` | Conversion |
| `lua_toboolean` | `0x7B82F0` | Conversion |
| `lua_tostring` | `0x7B8330` | Conversion |
| `lua_strlen` | `0x7B83A0` | Conversion |
| `lua_tocfunction` | `0x7B8400` | Conversion |
| `lua_touserdata` | `0x7B8440` | Conversion |
| `lua_tothread` | `0x7B8490` | Conversion |
| `lua_topointer` | `0x7B84D0` | Conversion |
| `lua_pushnil` | `0x7B8540` | Push |
| `lua_pushnumber` | `0x7B8560` | Push |
| `lua_pushlstring` | `0x7B8580` | Push |
| `lua_pushstring` | `0x7B85D0` | Push |
| `lua_pushvfstring` | `0x7B8640` | Push |
| `lua_pushfstring` | `0x7B8670` | Push |
| `lua_pushcclosure` | `0x7B86A0` | Push |
| `lua_pushboolean` | `0x7B8720` | Push |
| `lua_pushlightuserdata` | `0x7B8750` | Push |
| `lua_gettable` | `0x7B8770` | Get |
| `lua_rawget` | `0x7B87C0` | Get |
| `lua_rawgeti` | `0x7B8810` | Get |
| `lua_newtable` | `0x7B8860` | Get |
| `lua_getmetatable` | `0x7B88A0` | Get |
| `lua_getfenv` | `0x7B8910` | Get |
| `lua_settable` | `0x7B8960` | Set |
| `lua_rawset` | `0x7B89A0` | Set |
| `lua_rawseti` | `0x7B89F0` | Set |
| `lua_setmetatable` | `0x7B8A40` | Set |
| `lua_setfenv` | `0x7B8AB0` | Set |
| `lua_call` | `0x7B8B10` | Call |
| `lua_pcall` | `0x7B8B60` | Call |
| `lua_cpcall` | `0x7B8C60` | Call |
| `lua_load` | `0x7B8CA0` | Call |
| `lua_dump` | `0x7B8CF0` | Call |
| `lua_error` | `0x7B8DB0` | Misc |
| `lua_next` | `0x7B8DC0` | Misc |
| `lua_concat` | `0x7B8E10` | Misc |
| `lua_newuserdata` | `0x7B8E90` | Misc |
| `lua_getgcthreshold` | `0x7B8D40` | GC |
| `lua_getgccount` | `0x7B8D50` | GC |
| `lua_setgcthreshold` | `0x7B8D60` | GC |
| `lua_getupvalue` | `0x7B8FB0` | Debug |
| `lua_setupvalue` | `0x7B9030` | Debug |
| `lua_getinfo` | `0x7BED30` | Debug |
| `lua_pushupvalues` | `0x7B8EE0` | Debug |
| `lua_sethook` | `0x7BE0F0` | Debug |
| `lua_gethook` | `0x7BE130` | Debug |
| `lua_gethookmask` | `0x7BE140` | Debug |
| `lua_gethookcount` | `0x7BE150` | Debug |
| `lua_getstack` | `0x7BE160` | Debug |
| `lua_getlocal` | `0x7BE200` | Debug |
| `lua_setlocal` | `0x7BE280` | Debug |
| `lua_version` | `0x7B8DA0` | Compat |
| `lua_dofile` | `0x7B7870` | Compat |
| `lua_dobuffer` | `0x7B78B0` | Compat |
| `lua_dostring` | `0x7B7910` | Compat |

#### luaL_ Auxiliary Library

| Function | Address |
|----------|---------|
| `luaL_openlib` | `0x7B6DB0` |
| `luaL_callmeta` | `0x7B6D50` |
| `luaL_typerror` | `0x7B7A00` |
| `luaL_argerror` | `0x7B7940` |
| `luaL_checklstring` | `0x7B7B00` |
| `luaL_optlstring` | `0x7B7B70` |
| `luaL_checknumber` | `0x7B7BD0` |
| `luaL_optnumber` | `0x7B7C50` |
| `luaL_checktype` | `0x7B7A80` |
| `luaL_checkany` | `0x7B7AD0` |
| `luaL_newmetatable` | `0x7B6B90` |
| `luaL_getmetatable` | `0x7B6C10` |
| `luaL_checkudata` | `0x7B6C30` |
| `luaL_where` | `0x7B6A70` |
| `luaL_error` | `0x7B6B00` |
| `luaL_findstring` | `0x7B6B30` |
| `luaL_ref` | `0x7B73E0` |
| `luaL_unref` | `0x7B74B0` |
| `luaL_getn` | `0x7B7030` |
| `luaL_setn` | `0x7B6F60` |
| `luaL_loadfile` | `0x7B7590` |
| `luaL_loadbuffer` | `0x7B77A0` |
| `luaL_checkstack` | `0x7B6CC0` |
| `luaL_getmetafield` | `0x7B6CF0` |
| `luaL_buffinit` | `0x7B73C0` |
| `luaL_prepbuffer` | `0x7B7200` |
| `luaL_addlstring` | `0x7B7240` |
| `luaL_addstring` | `0x7B72A0` |
| `luaL_addvalue` | `0x7B7320` |
| `luaL_pushresult` | `0x7B72D0` |

#### luaopen_ Library Openers

| Function | Address |
|----------|---------|
| `luaopen_base` | `0x7BDB40` |
| `luaopen_table` | `0x7BC870` |
| `luaopen_io` | `0x7BBED0` |
| `luaopen_string` | `0x7BBDE0` |
| `luaopen_math` | `0x7BA390` |
| `luaopen_debug` | `0x7B9BD0` |
