#pragma once

#include "pch.h"
#include <cstdarg>

// =============================================================================
// SWBF2 Lua 5.0 Bindings
// =============================================================================
// SWBF2 (2005) uses Lua 5.0.2 with lua_Number defined as float (not double).
// These declarations mirror the Lua 5.0 C API so we can call the functions
// at their known in-exe addresses.

// Opaque Lua state — we only ever pass pointers.
struct lua_State;

// Opaque luaL_Buffer — forward-declared for buffer function pointers.
struct luaL_Buffer;

// The C function signature Lua expects: int fn(lua_State*)
using lua_CFunction = int(__cdecl*)(lua_State*);

// SWBF2 compiles Lua with float, not double.
using lua_Number = float;

// Chunk reader/writer callbacks for lua_load / lua_dump
using lua_Chunkreader = const char*(__cdecl*)(lua_State*, void*, size_t*);
using lua_Chunkwriter = int(__cdecl*)(lua_State*, const void*, size_t, void*);

// luaL_openlib registration entry
struct luaL_reg {
   const char* name;
   lua_CFunction func;
};

// =============================================================================
// Lua 5.0 Constants
// =============================================================================

constexpr int LUA_REGISTRYINDEX = -10000;
constexpr int LUA_GLOBALSINDEX  = -10001;

// Type tags (lua_type return values)
constexpr int LUA_TNONE          = -1;
constexpr int LUA_TNIL           = 0;
constexpr int LUA_TBOOLEAN       = 1;
constexpr int LUA_TLIGHTUSERDATA = 2;
constexpr int LUA_TNUMBER        = 3;
constexpr int LUA_TSTRING        = 4;
constexpr int LUA_TTABLE         = 5;
constexpr int LUA_TFUNCTION      = 6;
constexpr int LUA_TUSERDATA      = 7;
constexpr int LUA_TTHREAD        = 8;

// Reference system constants
constexpr int LUA_NOREF  = -2;
constexpr int LUA_REFNIL = -1;

// Hook event codes
constexpr int LUA_HOOKCALL    = 0;
constexpr int LUA_HOOKRET     = 1;
constexpr int LUA_HOOKLINE    = 2;
constexpr int LUA_HOOKCOUNT   = 3;

// Hook event masks
constexpr int LUA_MASKCALL    = (1 << LUA_HOOKCALL);
constexpr int LUA_MASKRET     = (1 << LUA_HOOKRET);
constexpr int LUA_MASKLINE    = (1 << LUA_HOOKLINE);
constexpr int LUA_MASKCOUNT   = (1 << LUA_HOOKCOUNT);

// =============================================================================
// lua_ function pointer types — Core C API
// =============================================================================

// -- State --
using fn_lua_open              = lua_State*(__cdecl*)();
using fn_lua_close             = void(__cdecl*)(lua_State*);
using fn_lua_newthread         = lua_State*(__cdecl*)(lua_State*);
using fn_lua_atpanic           = lua_CFunction(__cdecl*)(lua_State*, lua_CFunction);

// -- Stack manipulation --
using fn_lua_gettop            = int(__cdecl*)(lua_State*);
using fn_lua_settop            = void(__cdecl*)(lua_State*, int);
using fn_lua_pushvalue         = void(__cdecl*)(lua_State*, int);
using fn_lua_remove            = void(__cdecl*)(lua_State*, int);
using fn_lua_insert            = void(__cdecl*)(lua_State*, int);
using fn_lua_replace           = void(__cdecl*)(lua_State*, int);
using fn_lua_checkstack        = int(__cdecl*)(lua_State*, int);
using fn_lua_xmove             = void(__cdecl*)(lua_State*, lua_State*, int);

// -- Type checking --
using fn_lua_type              = int(__cdecl*)(lua_State*, int);
using fn_lua_typename          = const char*(__cdecl*)(lua_State*, int);
using fn_lua_isnumber          = int(__cdecl*)(lua_State*, int);
using fn_lua_isstring          = int(__cdecl*)(lua_State*, int);
using fn_lua_iscfunction       = int(__cdecl*)(lua_State*, int);
using fn_lua_isuserdata        = int(__cdecl*)(lua_State*, int);
using fn_lua_equal             = int(__cdecl*)(lua_State*, int, int);
using fn_lua_rawequal          = int(__cdecl*)(lua_State*, int, int);
using fn_lua_lessthan          = int(__cdecl*)(lua_State*, int, int);

// -- Conversion --
using fn_lua_tonumber          = float(__cdecl*)(lua_State*, int);
using fn_lua_toboolean         = int(__cdecl*)(lua_State*, int);
using fn_lua_tostring          = const char*(__cdecl*)(lua_State*, int);
using fn_lua_strlen            = size_t(__cdecl*)(lua_State*, int);
using fn_lua_tocfunction       = lua_CFunction(__cdecl*)(lua_State*, int);
using fn_lua_touserdata        = void*(__cdecl*)(lua_State*, int);
using fn_lua_tothread          = lua_State*(__cdecl*)(lua_State*, int);
using fn_lua_topointer         = const void*(__cdecl*)(lua_State*, int);

// -- Push --
using fn_lua_pushnil           = void(__cdecl*)(lua_State*);
using fn_lua_pushnumber        = void(__cdecl*)(lua_State*, float);
using fn_lua_pushlstring       = void(__cdecl*)(lua_State*, const char*, size_t);
using fn_lua_pushstring        = void(__cdecl*)(lua_State*, const char*);
using fn_lua_pushvfstring      = const char*(__cdecl*)(lua_State*, const char*, va_list);
using fn_lua_pushfstring       = const char*(__cdecl*)(lua_State*, const char*, ...);
using fn_lua_pushcclosure      = void(__cdecl*)(lua_State*, lua_CFunction, int);
using fn_lua_pushboolean       = void(__cdecl*)(lua_State*, int);
using fn_lua_pushlightuserdata = void(__cdecl*)(lua_State*, void*);

// -- Get --
using fn_lua_gettable          = void(__cdecl*)(lua_State*, int);
using fn_lua_rawget            = void(__cdecl*)(lua_State*, int);
using fn_lua_rawgeti           = void(__cdecl*)(lua_State*, int, int);
using fn_lua_newtable          = void(__cdecl*)(lua_State*);
using fn_lua_getmetatable      = int(__cdecl*)(lua_State*, int);
using fn_lua_getfenv           = void(__cdecl*)(lua_State*, int);

// -- Set --
using fn_lua_settable          = void(__cdecl*)(lua_State*, int);
using fn_lua_rawset            = void(__cdecl*)(lua_State*, int);
using fn_lua_rawseti           = void(__cdecl*)(lua_State*, int, int);
using fn_lua_setmetatable      = int(__cdecl*)(lua_State*, int);
using fn_lua_setfenv           = int(__cdecl*)(lua_State*, int);

// -- Call --
using fn_lua_call              = void(__cdecl*)(lua_State*, int, int);
using fn_lua_pcall             = int(__cdecl*)(lua_State*, int, int, int);
using fn_lua_cpcall            = int(__cdecl*)(lua_State*, lua_CFunction, void*);
using fn_lua_load              = int(__cdecl*)(lua_State*, lua_Chunkreader, void*, const char*);
using fn_lua_dump              = int(__cdecl*)(lua_State*, lua_Chunkwriter, void*);

// -- Misc --
using fn_lua_error             = int(__cdecl*)(lua_State*);
using fn_lua_next              = int(__cdecl*)(lua_State*, int);
using fn_lua_concat            = void(__cdecl*)(lua_State*, int);
using fn_lua_newuserdata       = void*(__cdecl*)(lua_State*, size_t);

// -- GC --
using fn_lua_getgcthreshold    = int(__cdecl*)(lua_State*);
using fn_lua_getgccount        = int(__cdecl*)(lua_State*);
using fn_lua_setgcthreshold    = void(__cdecl*)(lua_State*, int);

// -- Debug / upvalues --
using fn_lua_getupvalue        = const char*(__cdecl*)(lua_State*, int, int);
using fn_lua_setupvalue        = const char*(__cdecl*)(lua_State*, int, int);
using fn_lua_getinfo           = int(__cdecl*)(lua_State*, const char*, void*);
using fn_lua_pushupvalues      = int(__cdecl*)(lua_State*);
using fn_lua_sethook           = int(__cdecl*)(lua_State*, lua_CFunction, int, int);
using fn_lua_gethook           = lua_CFunction(__cdecl*)(lua_State*);
using fn_lua_gethookmask       = int(__cdecl*)(lua_State*);
using fn_lua_gethookcount      = int(__cdecl*)(lua_State*);
using fn_lua_getstack          = int(__cdecl*)(lua_State*, int, void*);
using fn_lua_getlocal          = const char*(__cdecl*)(lua_State*, void*, int);
using fn_lua_setlocal          = const char*(__cdecl*)(lua_State*, void*, int);

// -- Compat (Lua 5.0) --
using fn_lua_version           = const char*(__cdecl*)();
using fn_lua_dofile            = int(__cdecl*)(lua_State*, const char*);
using fn_lua_dobuffer          = int(__cdecl*)(lua_State*, const char*, size_t, const char*);
using fn_lua_dostring          = int(__cdecl*)(lua_State*, const char*);

// =============================================================================
// luaL_ function pointer types — Auxiliary library
// =============================================================================

using fn_luaL_openlib          = void(__cdecl*)(lua_State*, const char*, const luaL_reg*, int);
using fn_luaL_callmeta         = int(__cdecl*)(lua_State*, int, const char*);
using fn_luaL_typerror         = int(__cdecl*)(lua_State*, int, const char*);
using fn_luaL_argerror         = int(__cdecl*)(lua_State*, int, const char*);
using fn_luaL_checklstring     = const char*(__cdecl*)(lua_State*, int, size_t*);
using fn_luaL_optlstring       = const char*(__cdecl*)(lua_State*, int, const char*, size_t*);
using fn_luaL_checknumber      = float(__cdecl*)(lua_State*, int);
using fn_luaL_optnumber        = float(__cdecl*)(lua_State*, int, float);
using fn_luaL_checktype        = void(__cdecl*)(lua_State*, int, int);
using fn_luaL_checkany         = void(__cdecl*)(lua_State*, int);
using fn_luaL_newmetatable     = int(__cdecl*)(lua_State*, const char*);
using fn_luaL_getmetatable     = void(__cdecl*)(lua_State*, const char*);
using fn_luaL_checkudata       = void*(__cdecl*)(lua_State*, int, const char*);
using fn_luaL_where            = void(__cdecl*)(lua_State*, int);
using fn_luaL_error            = int(__cdecl*)(lua_State*, const char*, ...);
using fn_luaL_findstring       = int(__cdecl*)(const char*, const char* const[]);
using fn_luaL_ref              = int(__cdecl*)(lua_State*, int);
using fn_luaL_unref            = void(__cdecl*)(lua_State*, int, int);
using fn_luaL_getn             = int(__cdecl*)(lua_State*, int);
using fn_luaL_setn             = void(__cdecl*)(lua_State*, int, int);
using fn_luaL_loadfile         = int(__cdecl*)(lua_State*, const char*);
using fn_luaL_loadbuffer       = int(__cdecl*)(lua_State*, const char*, size_t, const char*);
using fn_luaL_checkstack       = void(__cdecl*)(lua_State*, int, const char*);
using fn_luaL_getmetafield     = int(__cdecl*)(lua_State*, int, const char*);

// -- Buffer --
using fn_luaL_buffinit         = void(__cdecl*)(lua_State*, luaL_Buffer*);
using fn_luaL_prepbuffer       = char*(__cdecl*)(luaL_Buffer*);
using fn_luaL_addlstring       = void(__cdecl*)(luaL_Buffer*, const char*, size_t);
using fn_luaL_addstring        = void(__cdecl*)(luaL_Buffer*, const char*);
using fn_luaL_addvalue         = void(__cdecl*)(luaL_Buffer*);
using fn_luaL_pushresult       = void(__cdecl*)(luaL_Buffer*);

// =============================================================================
// luaopen_ function pointer types — Library openers
// =============================================================================

using fn_luaopen               = int(__cdecl*)(lua_State*);

// =============================================================================
// Addresses — modtools & steam
// =============================================================================

namespace lua_addrs {
   namespace modtools {
      // -- Engine hooks --
      constexpr uintptr_t init_state              = 0x486660;
      constexpr uintptr_t g_lua_state_ptr         = 0xB35A58;

      // -- lua_ State --
      constexpr uintptr_t lua_open                = 0x7BDF50;
      constexpr uintptr_t lua_close               = 0x7BDFE0;
      constexpr uintptr_t lua_newthread           = 0x7B7E20;
      constexpr uintptr_t lua_atpanic             = 0x7B7E00;

      // -- lua_ Stack --
      constexpr uintptr_t lua_gettop              = 0x7B7E60;
      constexpr uintptr_t lua_settop              = 0x7B7E70;
      constexpr uintptr_t lua_pushvalue           = 0x7B7FB0;
      constexpr uintptr_t lua_remove              = 0x7B7ED0;
      constexpr uintptr_t lua_insert              = 0x7B7F20;
      constexpr uintptr_t lua_replace             = 0x7B7F70;
      constexpr uintptr_t lua_checkstack          = 0x7B7D50;
      constexpr uintptr_t lua_xmove              = 0x7B7DB0;

      // -- lua_ Type checking --
      constexpr uintptr_t lua_type                = 0x7B7FE0;
      constexpr uintptr_t lua_typename            = 0x7B8010;
      constexpr uintptr_t lua_isnumber            = 0x7B8070;
      constexpr uintptr_t lua_isstring            = 0x7B80C0;
      constexpr uintptr_t lua_iscfunction         = 0x7B8030;
      constexpr uintptr_t lua_isuserdata          = 0x7B8100;
      constexpr uintptr_t lua_equal               = 0x7B81B0;
      constexpr uintptr_t lua_rawequal            = 0x7B8140;
      constexpr uintptr_t lua_lessthan            = 0x7B8230;

      // -- lua_ Conversion --
      constexpr uintptr_t lua_tonumber            = 0x7B82A0;
      constexpr uintptr_t lua_toboolean           = 0x7B82F0;
      constexpr uintptr_t lua_tostring            = 0x7B8330;
      constexpr uintptr_t lua_strlen              = 0x7B83A0;
      constexpr uintptr_t lua_tocfunction         = 0x7B8400;
      constexpr uintptr_t lua_touserdata          = 0x7B8440;
      constexpr uintptr_t lua_tothread            = 0x7B8490;
      constexpr uintptr_t lua_topointer           = 0x7B84D0;

      // -- lua_ Push --
      constexpr uintptr_t lua_pushnil             = 0x7B8540;
      constexpr uintptr_t lua_pushnumber          = 0x7B8560;
      constexpr uintptr_t lua_pushlstring         = 0x7B8580;
      constexpr uintptr_t lua_pushstring          = 0x7B85D0;
      constexpr uintptr_t lua_pushvfstring        = 0x7B8640;
      constexpr uintptr_t lua_pushfstring         = 0x7B8670;
      constexpr uintptr_t lua_pushcclosure        = 0x7B86A0;
      constexpr uintptr_t lua_pushboolean         = 0x7B8720;
      constexpr uintptr_t lua_pushlightuserdata   = 0x7B8750;

      // -- lua_ Get --
      constexpr uintptr_t lua_gettable            = 0x7B8770;
      constexpr uintptr_t lua_rawget              = 0x7B87C0;
      constexpr uintptr_t lua_rawgeti             = 0x7B8810;
      constexpr uintptr_t lua_newtable            = 0x7B8860;
      constexpr uintptr_t lua_getmetatable        = 0x7B88A0;
      constexpr uintptr_t lua_getfenv             = 0x7B8910;

      // -- lua_ Set --
      constexpr uintptr_t lua_settable            = 0x7B8960;
      constexpr uintptr_t lua_rawset              = 0x7B89A0;
      constexpr uintptr_t lua_rawseti             = 0x7B89F0;
      constexpr uintptr_t lua_setmetatable        = 0x7B8A40;
      constexpr uintptr_t lua_setfenv             = 0x7B8AB0;

      // -- lua_ Call --
      constexpr uintptr_t lua_call                = 0x7B8B10;
      constexpr uintptr_t lua_pcall               = 0x7B8B60;
      constexpr uintptr_t lua_cpcall              = 0x7B8C60;
      constexpr uintptr_t lua_load                = 0x7B8CA0;
      constexpr uintptr_t lua_dump                = 0x7B8CF0;

      // -- lua_ Misc --
      constexpr uintptr_t lua_error               = 0x7B8DB0;
      constexpr uintptr_t lua_next                = 0x7B8DC0;
      constexpr uintptr_t lua_concat              = 0x7B8E10;
      constexpr uintptr_t lua_newuserdata         = 0x7B8E90;

      // -- lua_ GC --
      constexpr uintptr_t lua_getgcthreshold      = 0x7B8D40;
      constexpr uintptr_t lua_getgccount          = 0x7B8D50;
      constexpr uintptr_t lua_setgcthreshold      = 0x7B8D60;

      // -- lua_ Debug --
      constexpr uintptr_t lua_getupvalue          = 0x7B8FB0;
      constexpr uintptr_t lua_setupvalue          = 0x7B9030;
      constexpr uintptr_t lua_getinfo             = 0x7BED30;
      constexpr uintptr_t lua_pushupvalues        = 0x7B8EE0;
      constexpr uintptr_t lua_sethook             = 0x7BE0F0;
      constexpr uintptr_t lua_gethook             = 0x7BE130;
      constexpr uintptr_t lua_gethookmask         = 0x7BE140;
      constexpr uintptr_t lua_gethookcount        = 0x7BE150;
      constexpr uintptr_t lua_getstack            = 0x7BE160;
      constexpr uintptr_t lua_getlocal            = 0x7BE200;
      constexpr uintptr_t lua_setlocal            = 0x7BE280;

      // -- lua_ Compat --
      constexpr uintptr_t lua_version             = 0x7B8DA0;
      constexpr uintptr_t lua_dofile              = 0x7B7870;
      constexpr uintptr_t lua_dobuffer            = 0x7B78B0;
      constexpr uintptr_t lua_dostring            = 0x7B7910;

      // -- luaL_ Auxiliary --
      constexpr uintptr_t luaL_openlib            = 0x7B6DB0;
      constexpr uintptr_t luaL_callmeta           = 0x7B6D50;
      constexpr uintptr_t luaL_typerror           = 0x7B7A00;
      constexpr uintptr_t luaL_argerror           = 0x7B7940;
      constexpr uintptr_t luaL_checklstring       = 0x7B7B00;
      constexpr uintptr_t luaL_optlstring         = 0x7B7B70;
      constexpr uintptr_t luaL_checknumber        = 0x7B7BD0;
      constexpr uintptr_t luaL_optnumber          = 0x7B7C50;
      constexpr uintptr_t luaL_checktype          = 0x7B7A80;
      constexpr uintptr_t luaL_checkany           = 0x7B7AD0;
      constexpr uintptr_t luaL_newmetatable       = 0x7B6B90;
      constexpr uintptr_t luaL_getmetatable       = 0x7B6C10;
      constexpr uintptr_t luaL_checkudata         = 0x7B6C30;
      constexpr uintptr_t luaL_where              = 0x7B6A70;
      constexpr uintptr_t luaL_error              = 0x7B6B00;
      constexpr uintptr_t luaL_findstring         = 0x7B6B30;
      constexpr uintptr_t luaL_ref                = 0x7B73E0;
      constexpr uintptr_t luaL_unref              = 0x7B74B0;
      constexpr uintptr_t luaL_getn               = 0x7B7030;
      constexpr uintptr_t luaL_setn               = 0x7B6F60;
      constexpr uintptr_t luaL_loadfile           = 0x7B7590;
      constexpr uintptr_t luaL_loadbuffer         = 0x7B77A0;
      constexpr uintptr_t luaL_checkstack         = 0x7B6CC0;
      constexpr uintptr_t luaL_getmetafield       = 0x7B6CF0;

      // -- luaL_ Buffer --
      constexpr uintptr_t luaL_buffinit           = 0x7B73C0;
      constexpr uintptr_t luaL_prepbuffer         = 0x7B7200;
      constexpr uintptr_t luaL_addlstring         = 0x7B7240;
      constexpr uintptr_t luaL_addstring          = 0x7B72A0;
      constexpr uintptr_t luaL_addvalue           = 0x7B7320;
      constexpr uintptr_t luaL_pushresult         = 0x7B72D0;

      // -- luaopen_ Library openers --
      constexpr uintptr_t luaopen_base            = 0x7BDB40;
      constexpr uintptr_t luaopen_table           = 0x7BC870;
      constexpr uintptr_t luaopen_io              = 0x7BBED0;
      constexpr uintptr_t luaopen_string          = 0x7BBDE0;
      constexpr uintptr_t luaopen_math            = 0x7BA390;
      constexpr uintptr_t luaopen_debug           = 0x7B9BD0;

      // -- Game-specific --
      constexpr uintptr_t aimer_set_soldier_info              = 0x5EE9D0;
      constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0xA524D8;
      constexpr uintptr_t weapon_override_aimer_impl          = 0x61CEE0;
      constexpr uintptr_t weapon_override_aimer_thunk         = 0x4068DE;
      constexpr uintptr_t weapon_zoom_first_person            = 0x61B640;
   }

   namespace steam {
      // TODO: Fill in all addresses for the Steam exe.
      // Use the same constant names as the modtools namespace.
      constexpr uintptr_t init_state              = 0xDEAD0000;   // TODO
      constexpr uintptr_t g_lua_state_ptr         = 0xDEAD0000;   // TODO
      constexpr uintptr_t lua_pushcclosure        = 0xDEAD0002;   // TODO
      constexpr uintptr_t lua_pushlstring         = 0xDEAD0005;   // TODO
      constexpr uintptr_t lua_settable            = 0xDEAD0003;   // TODO
      constexpr uintptr_t luaL_checklstring       = 0xDEAD0004;   // TODO
      constexpr uintptr_t lua_pushnumber          = 0xDEAD0006;   // TODO
      constexpr uintptr_t lua_tonumber            = 0xDEAD0007;   // TODO
      constexpr uintptr_t lua_gettop              = 0xDEAD0008;   // TODO
      constexpr uintptr_t lua_pushnil             = 0xDEAD0009;   // TODO
      constexpr uintptr_t lua_pushboolean         = 0xDEAD000A;   // TODO
      constexpr uintptr_t lua_toboolean           = 0xDEAD000B;   // TODO
      constexpr uintptr_t lua_touserdata          = 0xDEAD000C;   // TODO
      constexpr uintptr_t lua_isnumber            = 0xDEAD000D;   // TODO
      constexpr uintptr_t aimer_set_soldier_info              = 0xDEAD000E;   // TODO
      constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0xDEAD000F;  // TODO
      constexpr uintptr_t weapon_override_aimer_impl          = 0xDEAD0010;   // TODO
      constexpr uintptr_t weapon_override_aimer_thunk         = 0xDEAD0011;   // TODO
   }
}

// =============================================================================
// Lua API struct — resolved at runtime from exe base + addresses above
// =============================================================================

struct lua_api {
   // ---- lua_ core API ----

   // State
   fn_lua_open              open              = nullptr;
   fn_lua_close             close             = nullptr;
   fn_lua_newthread         newthread         = nullptr;
   fn_lua_atpanic           atpanic           = nullptr;

   // Stack
   fn_lua_gettop            gettop            = nullptr;
   fn_lua_settop            settop            = nullptr;
   fn_lua_pushvalue         pushvalue         = nullptr;
   fn_lua_remove            remove            = nullptr;
   fn_lua_insert            insert            = nullptr;
   fn_lua_replace           replace           = nullptr;
   fn_lua_checkstack        checkstack        = nullptr;
   fn_lua_xmove             xmove             = nullptr;

   // Type checking
   fn_lua_type              type              = nullptr;
   fn_lua_typename          type_name         = nullptr;
   fn_lua_isnumber          isnumber          = nullptr;
   fn_lua_isstring          isstring          = nullptr;
   fn_lua_iscfunction       iscfunction       = nullptr;
   fn_lua_isuserdata        isuserdata        = nullptr;
   fn_lua_equal             equal             = nullptr;
   fn_lua_rawequal          rawequal          = nullptr;
   fn_lua_lessthan          lessthan          = nullptr;

   // Conversion
   fn_lua_tonumber          tonumber          = nullptr;
   fn_lua_toboolean         toboolean         = nullptr;
   fn_lua_tostring          tostring          = nullptr;
   fn_luaL_checklstring     tolstring         = nullptr;  // compat: actually luaL_checklstring
   fn_lua_strlen            str_len           = nullptr;
   fn_lua_tocfunction       tocfunction       = nullptr;
   fn_lua_touserdata        touserdata        = nullptr;
   fn_lua_tothread          tothread          = nullptr;
   fn_lua_topointer         topointer         = nullptr;

   // Push
   fn_lua_pushnil           pushnil           = nullptr;
   fn_lua_pushnumber        pushnumber        = nullptr;
   fn_lua_pushlstring       pushlstring       = nullptr;
   fn_lua_pushstring        pushstring        = nullptr;
   fn_lua_pushvfstring      pushvfstring      = nullptr;
   fn_lua_pushfstring       pushfstring       = nullptr;
   fn_lua_pushcclosure      pushcclosure      = nullptr;
   fn_lua_pushboolean       pushboolean       = nullptr;
   fn_lua_pushlightuserdata pushlightuserdata = nullptr;

   // Get
   fn_lua_gettable          gettable          = nullptr;
   fn_lua_rawget            rawget            = nullptr;
   fn_lua_rawgeti           rawgeti           = nullptr;
   fn_lua_newtable          newtable          = nullptr;
   fn_lua_getmetatable      getmetatable      = nullptr;
   fn_lua_getfenv           getfenv           = nullptr;

   // Set
   fn_lua_settable          settable          = nullptr;
   fn_lua_rawset            rawset            = nullptr;
   fn_lua_rawseti           rawseti           = nullptr;
   fn_lua_setmetatable      setmetatable      = nullptr;
   fn_lua_setfenv           setfenv           = nullptr;

   // Call
   fn_lua_call              call              = nullptr;
   fn_lua_pcall             pcall             = nullptr;
   fn_lua_cpcall            cpcall            = nullptr;
   fn_lua_load              load              = nullptr;
   fn_lua_dump              dump              = nullptr;

   // Misc
   fn_lua_error             error             = nullptr;
   fn_lua_next              next              = nullptr;
   fn_lua_concat            concat            = nullptr;
   fn_lua_newuserdata       newuserdata       = nullptr;

   // GC
   fn_lua_getgcthreshold    getgcthreshold    = nullptr;
   fn_lua_getgccount        getgccount        = nullptr;
   fn_lua_setgcthreshold    setgcthreshold    = nullptr;

   // Debug
   fn_lua_getupvalue        getupvalue        = nullptr;
   fn_lua_setupvalue        setupvalue        = nullptr;
   fn_lua_getinfo           getinfo           = nullptr;
   fn_lua_pushupvalues      pushupvalues      = nullptr;
   fn_lua_sethook           sethook           = nullptr;
   fn_lua_gethook           gethook           = nullptr;
   fn_lua_gethookmask       gethookmask       = nullptr;
   fn_lua_gethookcount      gethookcount      = nullptr;
   fn_lua_getstack          getstack          = nullptr;
   fn_lua_getlocal          getlocal          = nullptr;
   fn_lua_setlocal          setlocal          = nullptr;

   // Compat
   fn_lua_version           version           = nullptr;
   fn_lua_dofile            dofile            = nullptr;
   fn_lua_dobuffer          dobuffer          = nullptr;
   fn_lua_dostring          dostring          = nullptr;

   // ---- luaL_ auxiliary library ----
   fn_luaL_openlib          L_openlib         = nullptr;
   fn_luaL_callmeta         L_callmeta        = nullptr;
   fn_luaL_typerror         L_typerror        = nullptr;
   fn_luaL_argerror         L_argerror        = nullptr;
   fn_luaL_checklstring     L_checklstring    = nullptr;
   fn_luaL_optlstring       L_optlstring      = nullptr;
   fn_luaL_checknumber      L_checknumber     = nullptr;
   fn_luaL_optnumber        L_optnumber       = nullptr;
   fn_luaL_checktype        L_checktype       = nullptr;
   fn_luaL_checkany         L_checkany        = nullptr;
   fn_luaL_newmetatable     L_newmetatable    = nullptr;
   fn_luaL_getmetatable     L_getmetatable    = nullptr;
   fn_luaL_checkudata       L_checkudata      = nullptr;
   fn_luaL_where            L_where           = nullptr;
   fn_luaL_error            L_error           = nullptr;
   fn_luaL_findstring       L_findstring      = nullptr;
   fn_luaL_ref              L_ref             = nullptr;
   fn_luaL_unref            L_unref           = nullptr;
   fn_luaL_getn             L_getn            = nullptr;
   fn_luaL_setn             L_setn            = nullptr;
   fn_luaL_loadfile         L_loadfile        = nullptr;
   fn_luaL_loadbuffer       L_loadbuffer      = nullptr;
   fn_luaL_checkstack       L_checkstack      = nullptr;
   fn_luaL_getmetafield     L_getmetafield    = nullptr;

   // Buffer
   fn_luaL_buffinit         L_buffinit        = nullptr;
   fn_luaL_prepbuffer       L_prepbuffer      = nullptr;
   fn_luaL_addlstring       L_addlstring      = nullptr;
   fn_luaL_addstring        L_addstring       = nullptr;
   fn_luaL_addvalue         L_addvalue        = nullptr;
   fn_luaL_pushresult       L_pushresult      = nullptr;

   // Library openers
   fn_luaopen               open_base         = nullptr;
   fn_luaopen               open_table        = nullptr;
   fn_luaopen               open_io           = nullptr;
   fn_luaopen               open_string       = nullptr;
   fn_luaopen               open_math         = nullptr;
   fn_luaopen               open_debug        = nullptr;

   // ---- Helpers ----
   int tointeger(lua_State* L, int idx) const { return static_cast<int>(tonumber(L, idx)); }
};

// Global API instance — populated by lua_hooks_install()
extern lua_api g_lua;

// The captured lua_State pointer (set when hook fires)
extern lua_State* g_L;

// Barrel fire origin toggle — when true, WeaponCannon fires from barrel
// hardpoint (mBarrelPoseMatrix) instead of the default aimer position.
extern bool g_useBarrelFireOrigin;

// =============================================================================
// Public interface
// =============================================================================

// Call from install_patches() in dllmain.cpp.
void lua_hooks_install(uintptr_t exe_base);

// Cleanup (optional, called on DLL_PROCESS_DETACH if desired)
void lua_hooks_uninstall();

// Register a single C function as a named Lua global.
void lua_register_func(lua_State* L, const char* name, lua_CFunction fn);
