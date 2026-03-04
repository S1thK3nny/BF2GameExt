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
      // constexpr uintptr_t lua_close             = 0x7BDFE0;  // orphaned, stripped from retail
      constexpr uintptr_t lua_newthread           = 0x7B7E20;
      // constexpr uintptr_t lua_atpanic           = 0x7B7E00;  // orphaned, stripped from retail

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
      // constexpr uintptr_t lua_tocfunction       = 0x7B8400;  // orphaned, stripped from retail
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
      // constexpr uintptr_t lua_cpcall            = 0x7B8C60;  // orphaned, stripped from retail
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
      // constexpr uintptr_t lua_pushupvalues      = 0x7B8EE0;  // orphaned, stripped from retail
      constexpr uintptr_t lua_sethook             = 0x7BE0F0;
      constexpr uintptr_t lua_gethook             = 0x7BE130;
      constexpr uintptr_t lua_gethookmask         = 0x7BE140;
      constexpr uintptr_t lua_gethookcount        = 0x7BE150;
      constexpr uintptr_t lua_getstack            = 0x7BE160;
      constexpr uintptr_t lua_getlocal            = 0x7BE200;
      constexpr uintptr_t lua_setlocal            = 0x7BE280;

      // -- lua_ Compat --
      // constexpr uintptr_t lua_version           = 0x7B8DA0;  // orphaned, stripped from retail
      // constexpr uintptr_t lua_dofile            = 0x7B7870;  // orphaned, stripped from retail
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
      // constexpr uintptr_t luaL_checkudata       = 0x7B6C30;  // orphaned, stripped from retail
      constexpr uintptr_t luaL_where              = 0x7B6A70;
      constexpr uintptr_t luaL_error              = 0x7B6B00;
      // constexpr uintptr_t luaL_findstring       = 0x7B6B30;  // orphaned, stripped from retail
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
      // constexpr uintptr_t luaL_addstring        = 0x7B72A0;  // orphaned, stripped from retail
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
      constexpr uintptr_t close_state                         = 0x4866E0;
      constexpr uintptr_t weapon_signal_fire                  = 0x61C870;
      constexpr uintptr_t hash_to_name                        = 0x48DB10;
      constexpr uintptr_t pbl_hash_ctor                       = 0x7E1BD0;
      constexpr uintptr_t entity_class_registry               = 0xACD2C8;
      constexpr uintptr_t aimer_set_soldier_info              = 0x5EE9D0;
      constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0xA524D8;
      constexpr uintptr_t weapon_override_aimer_impl          = 0x61CEE0;
      constexpr uintptr_t weapon_override_aimer_thunk         = 0x4068DE;
      constexpr uintptr_t weapon_zoom_first_person            = 0x61B640;
      constexpr uintptr_t char_array_base_ptr                 = 0xB93A08;
      constexpr uintptr_t char_array_max_count                = 0xB939F4;
      constexpr uintptr_t team_array_ptr                      = 0xAD5D64;
      constexpr uintptr_t game_log                            = 0x7E3D50;

      // EntityFlyer::Update — JNP that skips fire suppression when mInLandingRegionFactor == 0.
      // Patching 0x7B (JNP) → 0xEB (JMP) allows firing in landing regions while flying.
      constexpr uintptr_t flyer_landing_fire_jnp             = 0x5004D3;

      // Weapon struct member offsets
      constexpr unsigned weapon_mLastFireTime_offset          = 0x11C;
      constexpr unsigned weapon_mClass_offset                 = 0x64;

      // Controllable struct member offsets
      constexpr unsigned controllable_mCharacter_offset       = 0xCC;
      constexpr unsigned controllable_mPilot_offset           = 0xD0;
      constexpr unsigned controllable_mPilotType_offset       = 0x144;
      constexpr unsigned controllable_mPlayerId_offset        = 0xD4;
      constexpr unsigned controllable_mIsAiming_offset         = 0x160;
      // Offsets from Controllable* into EntitySoldier derived fields
      constexpr unsigned controllable_to_soldierState_offset  = 0x514;
      constexpr unsigned controllable_to_soldierClass_offset  = 0x218;
      constexpr unsigned controllable_to_velocity_offset      = 0x2AC;
      // Offsets within EntitySoldierClass
      constexpr unsigned soldierClass_maxSpeed_offset         = 0x890;
      constexpr unsigned soldierClass_maxStrafe_offset        = 0x894;
      constexpr uintptr_t weapon_override_soldier_velocity     = 0x0061CEC0;
      constexpr uintptr_t weapon_update                        = 0x0061D850;
      constexpr uintptr_t char_exit_vehicle                    = 0x0052FC70;
      constexpr uintptr_t load_display_path_push_op            = 0x0067e388;
      constexpr uintptr_t load_display_dlc_flag_byte           = 0x0067e2ca;
      constexpr uintptr_t load_random_dlc_flag_byte            = 0x0067e05b;

      // ReadDataFile stricmp("ingame.lvl") — CALL __stricmp (direct E8, 5 bytes)
      constexpr uintptr_t ingame_stricmp_call                  = 0x0046a8f6;
      constexpr unsigned  ingame_stricmp_call_size             = 5;

      // Controller support — binding table setup
      // set_function_for_button (0x733520) and set_function_for_analog (0x7335a0)
      // write to 0x1ACC (Lua-only table) or are NOPs — not used.
      constexpr uintptr_t controller_base_global   = 0x00CAEB20;
      constexpr uintptr_t num_joysticks_global     = 0x00D2BDA8;
      constexpr uintptr_t joystick_config_base     = 0x00CB2A78;
      constexpr uintptr_t joystick_discover        = 0x007485f0; // thunk_FUN_007485f0 — discover joystick index
      constexpr uintptr_t joystick_sync            = 0x007489a0; // thunk_FUN_007489a0 — sync bindings + register device

      // Rumble output stubs — vanilla tick calls these with motor intensity.
      // Both are empty RET 4 on PC. Hook to route to XInput.
      constexpr uintptr_t rumble_light_output      = 0x0084ff00; // __stdcall(float intensity)
      constexpr uintptr_t rumble_heavy_output      = 0x0084fef0; // __thiscall(float intensity), ECX=device
      // Rumble state setup — dispatch calls this to populate rumble state from regions.
      // Empty RET 8 on PC. __thiscall(ECX=ctrl_base_global, int playerIdx, float* data)
      constexpr uintptr_t rumble_state_setup       = 0x007413a0;
      constexpr uintptr_t rumble_dispatch          = 0; // not needed — hook state_setup instead

      // Networking state globals (for MP guards — direct byte reads)
      constexpr uintptr_t net_in_shell       = 0x00ADABC2;
      constexpr uintptr_t net_enabled        = 0x00BE14F0;
      constexpr uintptr_t net_enabled_next   = 0x00BE14F1;
      constexpr uintptr_t net_on_client      = 0x00BE14FD;
   }

   namespace steam {
      // The following Lua 5.0.2 API functions are orphaned in the modtools exe
      // and were likely stripped by the retail compiler/linker:
      //   lua_close, lua_atpanic, lua_tocfunction, lua_cpcall, lua_pushupvalues,
      //   lua_version, lua_dofile, luaL_checkudata, luaL_findstring, luaL_addstring
      // If any are needed at runtime, check modtools addresses for reference:
      //   lua_close=0x7BDFE0  lua_atpanic=0x7B7E00  lua_tocfunction=0x7B8400
      //   lua_cpcall=0x7B8C60  lua_pushupvalues=0x7B8EE0  lua_version=0x7B8DA0
      //   lua_dofile=0x7B7870  luaL_checkudata=0x7B6C30  luaL_findstring=0x7B6B30
      //   luaL_addstring=0x7B72A0

      // -- Engine hooks --
      constexpr uintptr_t init_state              = 0x5A0CE0;
      constexpr uintptr_t g_lua_state_ptr         = 0x1E579A0;

      // -- lua_ State --
      constexpr uintptr_t lua_open                = 0x69F8E0;
      constexpr uintptr_t lua_close               = 0;   // likely stripped from retail
      constexpr uintptr_t lua_newthread           = 0x69BDF0;
      constexpr uintptr_t lua_atpanic             = 0;   // likely stripped from retail

      // -- lua_ Stack --
      constexpr uintptr_t lua_gettop              = 0x69BBA0;
      constexpr uintptr_t lua_settop              = 0x69C400;
      constexpr uintptr_t lua_pushvalue           = 0x69C0C0;
      constexpr uintptr_t lua_remove              = 0x69C260;
      constexpr uintptr_t lua_insert              = 0x69BC00;
      constexpr uintptr_t lua_replace             = 0x69C2A0;
      constexpr uintptr_t lua_checkstack          = 0x69B8E0;
      constexpr uintptr_t lua_xmove              = 0x69C6C0;

      // -- lua_ Type checking --
      constexpr uintptr_t lua_type                = 0x69C680;
      constexpr uintptr_t lua_typename            = 0x69C6A0;
      constexpr uintptr_t lua_isnumber            = 0x69BC80;
      constexpr uintptr_t lua_isstring            = 0x69BCC0;
      constexpr uintptr_t lua_iscfunction         = 0x69BC50;
      constexpr uintptr_t lua_isuserdata          = 0x69BCF0;
      constexpr uintptr_t lua_equal               = 0x69C120;
      constexpr uintptr_t lua_rawequal            = 0x69BD20;
      constexpr uintptr_t lua_lessthan            = 0x69BA00;

      // -- lua_ Conversion --
      constexpr uintptr_t lua_tonumber            = 0x69C510;
      constexpr uintptr_t lua_toboolean           = 0x69C4D0;
      constexpr uintptr_t lua_tostring            = 0x69C5B0;
      constexpr uintptr_t lua_strlen              = 0x69C490;
      constexpr uintptr_t lua_tocfunction         = 0;   // likely stripped from retail
      constexpr uintptr_t lua_touserdata          = 0x69C640;
      constexpr uintptr_t lua_tothread            = 0x69C610;
      constexpr uintptr_t lua_topointer           = 0x69C550;

      // -- lua_ Push --
      constexpr uintptr_t lua_pushnil             = 0x69C040;
      constexpr uintptr_t lua_pushnumber          = 0x69C060;
      constexpr uintptr_t lua_pushlstring         = 0x69C000;
      constexpr uintptr_t lua_pushstring          = 0x69C080;
      constexpr uintptr_t lua_pushvfstring        = 0x69C0F0;
      constexpr uintptr_t lua_pushfstring         = 0x69BFB0;
      constexpr uintptr_t lua_pushcclosure        = 0x69BF30;
      constexpr uintptr_t lua_pushboolean         = 0x69BF10;
      constexpr uintptr_t lua_pushlightuserdata   = 0x69BFE0;

      // -- lua_ Get --
      constexpr uintptr_t lua_gettable            = 0x69BB60;
      constexpr uintptr_t lua_rawget              = 0x69C160;
      constexpr uintptr_t lua_rawgeti             = 0x69C1A0;
      constexpr uintptr_t lua_newtable            = 0x69BDB0;
      constexpr uintptr_t lua_getmetatable        = 0x69BB00;
      constexpr uintptr_t lua_getfenv             = 0x69BA70;

      // -- lua_ Set --
      constexpr uintptr_t lua_settable            = 0x69C3D0;
      constexpr uintptr_t lua_rawset              = 0x69C1E0;
      constexpr uintptr_t lua_rawseti             = 0x69C220;
      constexpr uintptr_t lua_setmetatable        = 0x69C360;
      constexpr uintptr_t lua_setfenv             = 0x69C2D0;

      // -- lua_ Call --
      constexpr uintptr_t lua_call                = 0x69B8B0;
      constexpr uintptr_t lua_pcall               = 0x69BEB0;
      constexpr uintptr_t lua_cpcall              = 0;   // likely stripped from retail
      constexpr uintptr_t lua_load                = 0x69BD60;
      constexpr uintptr_t lua_dump                = 0x69B9C0;

      // -- lua_ Misc --
      constexpr uintptr_t lua_error               = 0x69BA50;
      constexpr uintptr_t lua_next                = 0x69BE70;
      constexpr uintptr_t lua_concat              = 0x69B940;
      constexpr uintptr_t lua_newuserdata         = 0x69BE30;

      // -- lua_ GC --
      constexpr uintptr_t lua_getgcthreshold      = 0x69BAE0;
      constexpr uintptr_t lua_getgccount          = 0x69BAC0;
      constexpr uintptr_t lua_setgcthreshold      = 0x69C320;

      // -- lua_ Debug --
      constexpr uintptr_t lua_getupvalue          = 0x69BBC0;
      constexpr uintptr_t lua_setupvalue          = 0x69C450;
      constexpr uintptr_t lua_getinfo             = 0x6A21A0;
      constexpr uintptr_t lua_pushupvalues        = 0;   // likely stripped from retail
      constexpr uintptr_t lua_sethook             = 0x6A2340;
      constexpr uintptr_t lua_gethook             = 0x6A2170;
      constexpr uintptr_t lua_gethookmask         = 0x6A2190;
      constexpr uintptr_t lua_gethookcount        = 0x6A2180;
      constexpr uintptr_t lua_getstack            = 0x6A22C0;
      constexpr uintptr_t lua_getlocal            = 0x6A2250;
      constexpr uintptr_t lua_setlocal            = 0x6A2380;

      // -- lua_ Compat --
      constexpr uintptr_t lua_version             = 0;   // likely stripped from retail
      constexpr uintptr_t lua_dofile              = 0;   // likely stripped from retail
      constexpr uintptr_t lua_dobuffer            = 0x69B700;
      constexpr uintptr_t lua_dostring            = 0x69B730;

      // -- luaL_ Auxiliary --
      constexpr uintptr_t luaL_openlib            = 0x69B260;
      constexpr uintptr_t luaL_callmeta           = 0x69AD30;
      constexpr uintptr_t luaL_typerror           = 0x69B5C0;
      constexpr uintptr_t luaL_argerror           = 0x69AC40;
      constexpr uintptr_t luaL_checklstring       = 0x69ADC0;
      constexpr uintptr_t luaL_optlstring         = 0x69B340;
      constexpr uintptr_t luaL_checknumber        = 0x69AE10;
      constexpr uintptr_t luaL_optnumber          = 0x69B3A0;
      constexpr uintptr_t luaL_checktype          = 0x69AE90;
      constexpr uintptr_t luaL_checkany           = 0x69AD90;
      constexpr uintptr_t luaL_newmetatable       = 0x69B1E0;
      constexpr uintptr_t luaL_getmetatable       = 0x69AF50;
      constexpr uintptr_t luaL_checkudata         = 0;   // likely stripped from retail
      constexpr uintptr_t luaL_where              = 0x69B670;
      constexpr uintptr_t luaL_error              = 0x69AEC0;
      constexpr uintptr_t luaL_findstring         = 0;   // likely stripped from retail
      constexpr uintptr_t luaL_ref                = 0x69B430;
      constexpr uintptr_t luaL_unref              = 0x69B600;
      constexpr uintptr_t luaL_getn               = 0x69AF70;
      constexpr uintptr_t luaL_setn               = 0x69B500;
      constexpr uintptr_t luaL_loadfile           = 0x69B060;
      constexpr uintptr_t luaL_loadbuffer         = 0x69B030;
      constexpr uintptr_t luaL_checkstack         = 0x69AE60;
      constexpr uintptr_t luaL_getmetafield       = 0x69AEF0;

      // -- luaL_ Buffer --
      constexpr uintptr_t luaL_buffinit           = 0x69AD10;
      constexpr uintptr_t luaL_prepbuffer         = 0x69B3D0;
      constexpr uintptr_t luaL_addlstring         = 0x69AB70;
      constexpr uintptr_t luaL_addstring          = 0;   // likely stripped/inlined from retail
      constexpr uintptr_t luaL_addvalue           = 0x69ABC0;
      constexpr uintptr_t luaL_pushresult         = 0x69B400;

      // -- luaopen_ Library openers --
      constexpr uintptr_t luaopen_base            = 0x6A0C00;
      constexpr uintptr_t luaopen_table           = 0x6A15B0;
      constexpr uintptr_t luaopen_io              = 0x6A0D90;
      constexpr uintptr_t luaopen_string          = 0x69ECD0;
      constexpr uintptr_t luaopen_math            = 0x69DA80;
      constexpr uintptr_t luaopen_debug           = 0x69D090;

      // -- Game-specific --
      constexpr uintptr_t close_state                         = 0x5A0D40;
      constexpr uintptr_t weapon_signal_fire                  = 0x679610;
      constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0x7B05EC;
      constexpr uintptr_t weapon_override_aimer_impl          = 0x677780;
      constexpr uintptr_t weapon_override_aimer_thunk         = 0;        // no thunk in retail
      constexpr uintptr_t weapon_zoom_first_person            = 0x677D40;
      constexpr uintptr_t flyer_landing_fire_jnp             = 0x4B019E;

      // Weapon struct member offsets (retail differs from modtools)
      constexpr unsigned weapon_mLastFireTime_offset          = 0xF8;
      constexpr unsigned weapon_mClass_offset                 = 0x64;

      // Controllable struct member offsets (same as modtools — no 4-byte shift)
      constexpr unsigned controllable_mCharacter_offset       = 0xCC;
      constexpr unsigned controllable_mPilot_offset           = 0xD0;
      constexpr unsigned controllable_mPilotType_offset       = 0x144;
      constexpr unsigned controllable_mPlayerId_offset        = 0xD4;
      constexpr unsigned controllable_mIsAiming_offset         = 0x160;
      // Offsets from Controllable* into EntitySoldier derived fields
      constexpr unsigned controllable_to_soldierState_offset  = 0x504;
      constexpr unsigned controllable_to_soldierClass_offset  = 0x200;
      constexpr unsigned controllable_to_velocity_offset      = 0x29C;
      // Offsets within EntitySoldierClass
      constexpr unsigned soldierClass_maxSpeed_offset         = 0x69C;
      constexpr unsigned soldierClass_maxStrafe_offset        = 0x6A0;
      constexpr uintptr_t weapon_override_soldier_velocity     = 0x00677760;
      constexpr uintptr_t weapon_update                        = 0x006781B0;

      constexpr uintptr_t pbl_hash_ctor                      = 0x726D20;
      constexpr uintptr_t hash_to_name                       = 0x652030;
      constexpr uintptr_t entity_class_registry              = 0x7EC560;
      constexpr uintptr_t char_array_base_ptr                = 0x1E30334;
      constexpr uintptr_t char_array_max_count               = 0x1E30330;
      constexpr uintptr_t team_array_ptr                     = 0x007E9AA0; // verified from SetReinforcementCount disasm
      constexpr uintptr_t game_log                           = 0x6F6FF0;  // verified: LogMessage(char*)
      constexpr uintptr_t char_exit_vehicle                  = 0x004F1380;
      constexpr uintptr_t load_display_path_push_op          = 0x00577661;
      constexpr uintptr_t load_display_dlc_flag_byte         = 0x0057764a;
      constexpr uintptr_t load_random_dlc_flag_byte          = 0x0057743c;

      // ReadDataFile stricmp("ingame.lvl") — CALL [IAT] (indirect FF15, 6 bytes)
      constexpr uintptr_t ingame_stricmp_call                = 0x0058adc4;
      constexpr unsigned  ingame_stricmp_call_size           = 6;

      // Controller support — binding table setup (found via ScriptCB_SetJoystickEnabled trace)
      constexpr uintptr_t controller_base_global   = 0x01EBDC50;
      constexpr uintptr_t num_joysticks_global     = 0x0099CD08;
      constexpr uintptr_t joystick_config_base     = 0x01EF90D0;
      constexpr uintptr_t joystick_discover        = 0x0061D250;
      constexpr uintptr_t joystick_sync            = 0x0061D590;

      // Rumble output stubs — vanilla tick calls these with motor intensity.
      constexpr uintptr_t rumble_light_output      = 0x006c5550;
      constexpr uintptr_t rumble_heavy_output      = 0x006c5540;
      constexpr uintptr_t rumble_state_setup       = 0; // stripped by compiler on Steam
      // Rumble dispatch — called from CameraManager::Update, checks rumble regions.
      // On modtools we hook the state setup stub instead. On Steam the stub was
      // optimized out, so we hook the dispatch itself to capture intensity.
      constexpr uintptr_t rumble_dispatch          = 0x00630d60;

      // Networking state globals (for MP guards — direct byte reads)
      constexpr uintptr_t net_in_shell       = 0x007E8007;
      constexpr uintptr_t net_enabled        = 0x01E62EA9;
      constexpr uintptr_t net_enabled_next   = 0x01E62EA8;
      constexpr uintptr_t net_on_client      = 0x01E62EAB;
   }

   namespace gog {
      // GOG exe: BattlefrontII_MemExt.exe
      // Lua library addresses are steam + 0x1090.
      // Game-specific addresses have variable offsets — filled in separately.
      //
      // Same stripped functions as steam (see steam namespace comment).

      // -- Engine hooks --
      constexpr uintptr_t init_state              = 0x5A1C90;
      constexpr uintptr_t g_lua_state_ptr         = 0x1E58E50;

      // -- lua_ State --
      constexpr uintptr_t lua_open                = 0x6A0970;
      constexpr uintptr_t lua_close               = 0;   // likely stripped from retail
      constexpr uintptr_t lua_newthread           = 0x69CE80;
      constexpr uintptr_t lua_atpanic             = 0;   // likely stripped from retail

      // -- lua_ Stack --
      constexpr uintptr_t lua_gettop              = 0x69CC30;
      constexpr uintptr_t lua_settop              = 0x69D490;
      constexpr uintptr_t lua_pushvalue           = 0x69D150;
      constexpr uintptr_t lua_remove              = 0x69D2F0;
      constexpr uintptr_t lua_insert              = 0x69CC90;
      constexpr uintptr_t lua_replace             = 0x69D330;
      constexpr uintptr_t lua_checkstack          = 0x69C970;
      constexpr uintptr_t lua_xmove              = 0x69D750;

      // -- lua_ Type checking --
      constexpr uintptr_t lua_type                = 0x69D710;
      constexpr uintptr_t lua_typename            = 0x69D730;
      constexpr uintptr_t lua_isnumber            = 0x69CD10;
      constexpr uintptr_t lua_isstring            = 0x69CD50;
      constexpr uintptr_t lua_iscfunction         = 0x69CCE0;
      constexpr uintptr_t lua_isuserdata          = 0x69CD80;
      constexpr uintptr_t lua_equal               = 0x69D1B0;
      constexpr uintptr_t lua_rawequal            = 0x69CDB0;
      constexpr uintptr_t lua_lessthan            = 0x69CA90;

      // -- lua_ Conversion --
      constexpr uintptr_t lua_tonumber            = 0x69D5A0;
      constexpr uintptr_t lua_toboolean           = 0x69D560;
      constexpr uintptr_t lua_tostring            = 0x69D640;
      constexpr uintptr_t lua_strlen              = 0x69D520;
      constexpr uintptr_t lua_tocfunction         = 0;   // likely stripped from retail
      constexpr uintptr_t lua_touserdata          = 0x69D6D0;
      constexpr uintptr_t lua_tothread            = 0x69D6A0;
      constexpr uintptr_t lua_topointer           = 0x69D5E0;

      // -- lua_ Push --
      constexpr uintptr_t lua_pushnil             = 0x69D0D0;
      constexpr uintptr_t lua_pushnumber          = 0x69D0F0;
      constexpr uintptr_t lua_pushlstring         = 0x69D090;
      constexpr uintptr_t lua_pushstring          = 0x69D110;
      constexpr uintptr_t lua_pushvfstring        = 0x69D180;
      constexpr uintptr_t lua_pushfstring         = 0x69D040;
      constexpr uintptr_t lua_pushcclosure        = 0x69CFC0;
      constexpr uintptr_t lua_pushboolean         = 0x69CFA0;
      constexpr uintptr_t lua_pushlightuserdata   = 0x69D070;

      // -- lua_ Get --
      constexpr uintptr_t lua_gettable            = 0x69CBF0;
      constexpr uintptr_t lua_rawget              = 0x69D1F0;
      constexpr uintptr_t lua_rawgeti             = 0x69D230;
      constexpr uintptr_t lua_newtable            = 0x69CE40;
      constexpr uintptr_t lua_getmetatable        = 0x69CB90;
      constexpr uintptr_t lua_getfenv             = 0x69CB00;

      // -- lua_ Set --
      constexpr uintptr_t lua_settable            = 0x69D460;
      constexpr uintptr_t lua_rawset              = 0x69D270;
      constexpr uintptr_t lua_rawseti             = 0x69D2B0;
      constexpr uintptr_t lua_setmetatable        = 0x69D3F0;
      constexpr uintptr_t lua_setfenv             = 0x69D360;

      // -- lua_ Call --
      constexpr uintptr_t lua_call                = 0x69C940;
      constexpr uintptr_t lua_pcall               = 0x69CF40;
      constexpr uintptr_t lua_cpcall              = 0;   // likely stripped from retail
      constexpr uintptr_t lua_load                = 0x69CDF0;
      constexpr uintptr_t lua_dump                = 0x69CA50;

      // -- lua_ Misc --
      constexpr uintptr_t lua_error               = 0x69CAE0;
      constexpr uintptr_t lua_next                = 0x69CF00;
      constexpr uintptr_t lua_concat              = 0x69C9D0;
      constexpr uintptr_t lua_newuserdata         = 0x69CEC0;

      // -- lua_ GC --
      constexpr uintptr_t lua_getgcthreshold      = 0x69CB70;
      constexpr uintptr_t lua_getgccount          = 0x69CB50;
      constexpr uintptr_t lua_setgcthreshold      = 0x69D3B0;

      // -- lua_ Debug --
      constexpr uintptr_t lua_getupvalue          = 0x69CC50;
      constexpr uintptr_t lua_setupvalue          = 0x69D4E0;
      constexpr uintptr_t lua_getinfo             = 0x6A3230;
      constexpr uintptr_t lua_pushupvalues        = 0;   // likely stripped from retail
      constexpr uintptr_t lua_sethook             = 0x6A33D0;
      constexpr uintptr_t lua_gethook             = 0x6A3200;
      constexpr uintptr_t lua_gethookmask         = 0x6A3220;
      constexpr uintptr_t lua_gethookcount        = 0x6A3210;
      constexpr uintptr_t lua_getstack            = 0x6A3350;
      constexpr uintptr_t lua_getlocal            = 0x6A32E0;
      constexpr uintptr_t lua_setlocal            = 0x6A3410;

      // -- lua_ Compat --
      constexpr uintptr_t lua_version             = 0;   // likely stripped from retail
      constexpr uintptr_t lua_dofile              = 0;   // likely stripped from retail
      constexpr uintptr_t lua_dobuffer            = 0x69C790;
      constexpr uintptr_t lua_dostring            = 0x69C7C0;

      // -- luaL_ Auxiliary --
      constexpr uintptr_t luaL_openlib            = 0x69C2F0;
      constexpr uintptr_t luaL_callmeta           = 0x69BDC0;
      constexpr uintptr_t luaL_typerror           = 0x69C650;
      constexpr uintptr_t luaL_argerror           = 0x69BCD0;
      constexpr uintptr_t luaL_checklstring       = 0x69BE50;
      constexpr uintptr_t luaL_optlstring         = 0x69C3D0;
      constexpr uintptr_t luaL_checknumber        = 0x69BEA0;
      constexpr uintptr_t luaL_optnumber          = 0x69C430;
      constexpr uintptr_t luaL_checktype          = 0x69BF20;
      constexpr uintptr_t luaL_checkany           = 0x69BE20;
      constexpr uintptr_t luaL_newmetatable       = 0x69C270;
      constexpr uintptr_t luaL_getmetatable       = 0x69BFE0;
      constexpr uintptr_t luaL_checkudata         = 0;   // likely stripped from retail
      constexpr uintptr_t luaL_where              = 0x69C700;
      constexpr uintptr_t luaL_error              = 0x69BF50;
      constexpr uintptr_t luaL_findstring         = 0;   // likely stripped from retail
      constexpr uintptr_t luaL_ref                = 0x69C4C0;
      constexpr uintptr_t luaL_unref              = 0x69C690;
      constexpr uintptr_t luaL_getn               = 0x69C000;
      constexpr uintptr_t luaL_setn               = 0x69C590;
      constexpr uintptr_t luaL_loadfile           = 0x69C0F0;
      constexpr uintptr_t luaL_loadbuffer         = 0x69C0C0;
      constexpr uintptr_t luaL_checkstack         = 0x69BEF0;
      constexpr uintptr_t luaL_getmetafield       = 0x69BF80;

      // -- luaL_ Buffer --
      constexpr uintptr_t luaL_buffinit           = 0x69BDA0;
      constexpr uintptr_t luaL_prepbuffer         = 0x69C460;
      constexpr uintptr_t luaL_addlstring         = 0x69BC00;
      constexpr uintptr_t luaL_addstring          = 0;   // likely stripped/inlined from retail
      constexpr uintptr_t luaL_addvalue           = 0x69BC50;
      constexpr uintptr_t luaL_pushresult         = 0x69C490;

      // -- luaopen_ Library openers --
      constexpr uintptr_t luaopen_base            = 0x6A1C90;
      constexpr uintptr_t luaopen_table           = 0x6A2640;
      constexpr uintptr_t luaopen_io              = 0x6A1E20;
      constexpr uintptr_t luaopen_string          = 0x69FD60;
      constexpr uintptr_t luaopen_math            = 0x69EB10;
      constexpr uintptr_t luaopen_debug           = 0x69E120;

      // -- Game-specific --
      constexpr uintptr_t close_state                         = 0x5A1CF0;
      constexpr uintptr_t weapon_signal_fire                  = 0x67A6B0;
      constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0x7B1564;
      constexpr uintptr_t weapon_override_aimer_impl          = 0x678820;
      constexpr uintptr_t weapon_override_aimer_thunk         = 0;        // no thunk in retail
      constexpr uintptr_t weapon_zoom_first_person            = 0x678DE0;
      constexpr uintptr_t flyer_landing_fire_jnp             = 0x4B0198;

      // Weapon struct member offsets (retail differs from modtools)
      constexpr unsigned weapon_mLastFireTime_offset          = 0xF8;
      constexpr unsigned weapon_mClass_offset                 = 0x64;

      // Controllable struct member offsets (same as modtools — no 4-byte shift)
      constexpr unsigned controllable_mCharacter_offset       = 0xCC;
      constexpr unsigned controllable_mPilot_offset           = 0xD0;
      constexpr unsigned controllable_mPilotType_offset       = 0x144;
      constexpr unsigned controllable_mPlayerId_offset        = 0xD4;
      constexpr unsigned controllable_mIsAiming_offset         = 0x160;
      // Offsets from Controllable* into EntitySoldier derived fields
      constexpr unsigned controllable_to_soldierState_offset  = 0x504;
      constexpr unsigned controllable_to_soldierClass_offset  = 0x200;
      constexpr unsigned controllable_to_velocity_offset      = 0x29C;
      // Offsets within EntitySoldierClass
      constexpr unsigned soldierClass_maxSpeed_offset         = 0x69C;
      constexpr unsigned soldierClass_maxStrafe_offset        = 0x6A0;
      constexpr uintptr_t weapon_override_soldier_velocity     = 0x00678800;
      constexpr uintptr_t weapon_update                        = 0x00679250;

      constexpr uintptr_t pbl_hash_ctor                      = 0x727DF0;
      constexpr uintptr_t hash_to_name                       = 0x6530D0;
      constexpr uintptr_t entity_class_registry              = 0x7ED4F0;
      constexpr uintptr_t char_array_base_ptr                = 0x1E317D4;
      constexpr uintptr_t char_array_max_count               = 0x1E317C0; // TODO: verify — likely 0x1E317D0 (Steam had same wrong-gap bug)
      constexpr uintptr_t team_array_ptr                     = 0;         // TODO
      constexpr uintptr_t game_log                           = 0;         // TODO
      constexpr uintptr_t char_exit_vehicle                  = 0x004F1380;
      constexpr uintptr_t load_display_path_push_op          = 0x005783e1;
      constexpr uintptr_t load_display_dlc_flag_byte         = 0x005783ca;
      constexpr uintptr_t load_random_dlc_flag_byte          = 0x005781bc;

      // ReadDataFile stricmp("ingame.lvl") — CALL [IAT] (indirect FF15, 6 bytes)
      constexpr uintptr_t ingame_stricmp_call                = 0x0058bd74;
      constexpr unsigned  ingame_stricmp_call_size           = 6;

      // Controller support — binding table setup (found via FUN_00637290 / joystick_sync xrefs)
      constexpr uintptr_t controller_base_global   = 0x01EBF100;  // DAT ref in FUN_00637290 (GOG equiv of Steam 0x01EBDC50)
      constexpr uintptr_t num_joysticks_global     = 0x0099E1A8;  // cleared in FUN_006dccd0 (GOG equiv of Steam 0x0099CD08)
      constexpr uintptr_t joystick_config_base     = 0x01EFA590;  // CMP EDI,0x1efa590 in FUN_0061e5f0 (GOG equiv of Steam 0x01EF90D0)
      constexpr uintptr_t joystick_discover        = 0x0061E2B0;  // byte-identical prologue to Steam 0x0061D250
      constexpr uintptr_t joystick_sync            = 0x0061E5F0;  // byte-identical prologue to Steam 0x0061D590

      // Rumble output stubs — vanilla tick calls these with motor intensity.
      // Both are empty RET 4 on PC. Hook to route to XInput.
      // light=0x006C65E0, heavy=0x006C65D0 confirmed via xref call-site matching Steam.
      constexpr uintptr_t rumble_light_output      = 0x006C65E0;
      constexpr uintptr_t rumble_heavy_output      = 0x006C65D0;
      constexpr uintptr_t rumble_state_setup       = 0; // stripped by compiler on GOG (same as Steam)
      // Rumble dispatch — called from CameraManager::Update, checks rumble regions.
      // GOG dispatch does not call a state_setup stub (inlined/stripped like Steam).
      constexpr uintptr_t rumble_dispatch          = 0x00631E00;

      // Networking state globals (for MP guards — direct byte reads)
      constexpr uintptr_t net_in_shell       = 0x007E9007;
      constexpr uintptr_t net_enabled        = 0x01E64359;
      constexpr uintptr_t net_enabled_next   = 0x01E64358;
      constexpr uintptr_t net_on_client      = 0x01E6435B;
   }
}

// =============================================================================
// Exe detection — identifies which build we're injected into
// =============================================================================

enum class ExeType { UNKNOWN, MODTOOLS, STEAM, GOG };

// =============================================================================
// Game-specific addresses — filled at startup from the correct namespace
// =============================================================================

struct game_addrs {
   uintptr_t init_state                         = 0;
   uintptr_t close_state                        = 0;
   uintptr_t g_lua_state_ptr                    = 0;
   uintptr_t weapon_signal_fire                 = 0;
   uintptr_t weapon_cannon_vftable_override_aimer = 0;
   uintptr_t weapon_override_aimer_impl         = 0;
   uintptr_t weapon_override_aimer_thunk        = 0;
   uintptr_t weapon_zoom_first_person           = 0;
   uintptr_t flyer_landing_fire_jnp             = 0;
   uintptr_t char_array_base_ptr                = 0;
   uintptr_t char_array_max_count               = 0;
   uintptr_t pbl_hash_ctor                      = 0;
   uintptr_t hash_to_name                       = 0;
   uintptr_t entity_class_registry              = 0;
   uintptr_t team_array_ptr                     = 0;
   uintptr_t game_log                           = 0;

   // Weapon struct member offsets (differ between modtools and retail)
   unsigned weapon_mLastFireTime_offset          = 0;
   unsigned weapon_mClass_offset                 = 0;

   // Controllable struct member offsets (retail is 4 bytes smaller than modtools)
   unsigned controllable_mCharacter_offset       = 0;
   unsigned controllable_mPilot_offset           = 0;
   unsigned controllable_mPilotType_offset       = 0;
   unsigned controllable_mPlayerId_offset        = 0;
   unsigned controllable_mIsAiming_offset        = 0;
   // Offsets from Controllable* into EntitySoldier derived fields
   unsigned controllable_to_soldierState_offset  = 0;
   unsigned controllable_to_soldierClass_offset  = 0;
   unsigned controllable_to_velocity_offset      = 0;
   // Offsets within EntitySoldierClass
   unsigned soldierClass_maxSpeed_offset         = 0;
   unsigned soldierClass_maxStrafe_offset        = 0;
   uintptr_t weapon_override_soldier_velocity    = 0;
   uintptr_t weapon_update                      = 0;
   uintptr_t char_exit_vehicle                  = 0;
   uintptr_t load_display_path_push_op           = 0;
   uintptr_t load_display_dlc_flag_byte          = 0;
   uintptr_t load_random_dlc_flag_byte           = 0;
   uintptr_t ingame_stricmp_call                 = 0;
   unsigned  ingame_stricmp_call_size             = 0;

   // Networking state (for MP guards — direct byte reads at resolved address)
   uintptr_t net_in_shell     = 0;
   uintptr_t net_enabled      = 0;
   uintptr_t net_enabled_next = 0;
   uintptr_t net_on_client    = 0;

   // Controller support — binding table setup
   uintptr_t controller_base_global   = 0;
   uintptr_t num_joysticks_global     = 0;
   uintptr_t joystick_config_base     = 0;
   uintptr_t joystick_discover        = 0;
   uintptr_t joystick_sync            = 0;

   // Rumble output stubs — hooked to route vanilla rumble to XInput
   uintptr_t rumble_light_output      = 0;
   uintptr_t rumble_heavy_output      = 0;
   uintptr_t rumble_state_setup       = 0;
   uintptr_t rumble_dispatch          = 0;

};

extern game_addrs g_game;
extern ExeType    g_exeType;
extern int        g_debugLogLevel;  // 0=off, 1=normal, 2=verbose

// Logging — defined in lua_hooks.cpp, usable from any TU
void dbg_log(const char* fmt, ...);
void dbg_log_verbose(const char* fmt, ...);

// =============================================================================
// Multiplayer state helpers
// =============================================================================
// Replicate the vanilla game's callback guard pattern using the same globals.
// Returns false (allow execution) if addresses aren't resolved yet.

inline bool isMultiplayer()
{
   if (!g_game.net_in_shell || !g_game.net_enabled || !g_game.net_enabled_next)
      return false;
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };
   char net = *(char*)res(g_game.net_in_shell)
            ? *(char*)res(g_game.net_enabled_next)
            : *(char*)res(g_game.net_enabled);
   return (net && *(char*)res(g_game.net_enabled));
}

inline bool isMultiplayerClient()
{
   if (!isMultiplayer()) return false;
   if (!g_game.net_on_client) return false;
   const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - 0x400000u + base; };
   return *(char*)res(g_game.net_on_client) != 0;
}

// =============================================================================
// Lua API struct — resolved at runtime from exe base + addresses above
// =============================================================================

struct lua_api {
   // ---- lua_ core API ----

   // State
   fn_lua_open              open              = nullptr;
   fn_lua_newthread         newthread         = nullptr;

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
   fn_lua_sethook           sethook           = nullptr;
   fn_lua_gethook           gethook           = nullptr;
   fn_lua_gethookmask       gethookmask       = nullptr;
   fn_lua_gethookcount      gethookcount      = nullptr;
   fn_lua_getstack          getstack          = nullptr;
   fn_lua_getlocal          getlocal          = nullptr;
   fn_lua_setlocal          setlocal          = nullptr;

   // Compat
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
   fn_luaL_where            L_where           = nullptr;
   fn_luaL_error            L_error           = nullptr;
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

extern char g_loadDisplayPath[260];
extern bool g_ingameInitArmed;
extern uint8_t* g_loadDisplay_dlc_flag_ptr;
extern uint8_t* g_loadRandom_dlc_flag_ptr;

// Per-character speed factor API — called from lua_funcs.cpp
void set_character_speed_factor(int charIndex, float factor, float durationSec, float lerpSpeed);
void set_character_aim_speed_factor(int charIndex, float factor, float lerpSpeed);
void set_character_fire_speed_factor(int charIndex, float factor, float cooldownSec, float chance, float lerpSpeed);
void clear_character_speed_factor(int charIndex);
void clear_fire_speed_factor(int charIndex);
void clear_all_character_speed_factors();
void clear_all_fire_speed_factors();

float get_entity_movement_speed(void* entity);

// =============================================================================
// Public interface
// =============================================================================

// Call from install_patches() in dllmain.cpp.
void lua_hooks_install(uintptr_t exe_base);
void lua_hooks_post_install();

// Cleanup (optional, called on DLL_PROCESS_DETACH if desired)
void lua_hooks_uninstall();

// Register a single C function as a named Lua global.
void lua_register_func(lua_State* L, const char* name, lua_CFunction fn);
