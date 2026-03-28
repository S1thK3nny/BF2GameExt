#pragma once

#include <stdint.h>

// =============================================================================
// Central address registry for all hooked/called game functions.
// Organized by target executable, then by game subsystem.
// All addresses are unrelocated (imagebase 0x400000).
// =============================================================================

namespace game_addrs {

// =============================================================================
// BF2_modtools
// =============================================================================
namespace modtools {

   // ---- Lua VM ---------------------------------------------------------------

   // LuaHelper::InitState — initializes Lua and registers all standard libs.
   constexpr uintptr_t init_state        = 0x486660;

   // Pointer to the exe's global lua_State* variable.
   constexpr uintptr_t g_lua_state_ptr   = 0xB35A58;

   constexpr uintptr_t lua_pushcclosure  = 0x7B86A0;
   constexpr uintptr_t lua_pushlstring   = 0x7B8580;
   constexpr uintptr_t lua_settable      = 0x7B8960;
   constexpr uintptr_t lua_tolstring     = 0x7B7B00;
   constexpr uintptr_t lua_pushnumber    = 0x7B8560;
   constexpr uintptr_t lua_tonumber      = 0x7B82A0;
   constexpr uintptr_t lua_gettop        = 0x7B7E60;
   constexpr uintptr_t lua_pushnil       = 0x7B8540;
   constexpr uintptr_t lua_pushboolean   = 0x7B8720;
   constexpr uintptr_t lua_toboolean     = 0x7B82F0;
   constexpr uintptr_t lua_touserdata    = 0x7B8440;
   constexpr uintptr_t lua_pushlightuserdata = 0x7B8750;
   constexpr uintptr_t lua_isnumber      = 0x7B8070;
   constexpr uintptr_t lua_gettable      = 0x7B89A0;
   constexpr uintptr_t lua_pcall         = 0x7B8B60;
   constexpr uintptr_t lua_rawgeti       = 0x7B8810;
   constexpr uintptr_t lua_settop        = 0x7B7E70;
   constexpr uintptr_t lua_insert        = 0x7B7F20;

   // ---- Aimer / Weapon -------------------------------------------------------

   // Aimer::SetSoldierInfo(Aimer*, PblVector3* pos, PblVector3* dir)
   constexpr uintptr_t aimer_set_soldier_info = 0x5EE9D0;

   // WeaponCannon vtable entry for OverrideAimer (vtable slot 0x70)
   constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0xA524D8;

   // Weapon::OverrideAimer implementation and thunk
   constexpr uintptr_t weapon_override_aimer_impl  = 0x61CEE0;
   constexpr uintptr_t weapon_override_aimer_thunk = 0x4068DE;

   // Weapon::ZoomFirstPerson() — returns true if weapon is in first-person zoom
   constexpr uintptr_t weapon_zoom_first_person = 0x61B640;

   // ---- Loading Screen (LoadDisplay) -----------------------------------------

   constexpr uintptr_t load_data_file_real       = 0x0067e2b0;
   constexpr uintptr_t load_config_real          = 0x0067c650;
   constexpr uintptr_t render_screen_real        = 0x0067a1b0;
   constexpr uintptr_t load_end_real             = 0x0067de10;
   constexpr uintptr_t progress_set_all_on       = 0x0040786f;
   constexpr uintptr_t load_update_real          = 0x0067c1d0;
   constexpr uintptr_t load_render_real          = 0x00402b71;
   constexpr uintptr_t load_update_qpc_stamp     = 0x00ba2f60;
   constexpr uintptr_t platform_render_texture   = 0x004165fe;

   // ---- PblConfig (config file parser) ---------------------------------------

   constexpr uintptr_t pbl_config_ctor           = 0x00821000;
   constexpr uintptr_t pbl_config_copy_ctor      = 0x00821080;
   constexpr uintptr_t pbl_read_next_data        = 0x008210f0;
   constexpr uintptr_t pbl_read_next_scope       = 0x00821140;

   // ---- Hashing / Texture lookup ---------------------------------------------

   // HashString raw (__cdecl, inner function of the __thiscall wrapper at 0x007e1bd0)
   constexpr uintptr_t hash_string               = 0x007e1b70;
   constexpr uintptr_t pbl_hash_table_find       = 0x007e1a40;
   constexpr uintptr_t tex_hash_table            = 0x00d4f994;
   constexpr uintptr_t color_ptr_global          = 0xae2150;

   // ---- Memory heap management -----------------------------------------------

   constexpr uintptr_t red_set_current_heap      = 0x007e2c70;
   constexpr uintptr_t runtime_heap_global       = 0x00b30220;
   constexpr uintptr_t s_loadheap_global         = 0x00ba111c;

   // ---- Sound (Snd::*) ------------------------------------------------------

   constexpr uintptr_t snd_find_by_hash_id       = 0x0088c500;
   constexpr uintptr_t snd_sound_play            = 0x0088cc10;
   constexpr uintptr_t gamesound_controllable_play = 0x0074dd30;
   constexpr uintptr_t voice_virtual_release       = 0x0074d440;
   constexpr uintptr_t voice_to_handle             = 0x0088b5d0;
   constexpr uintptr_t snd_engine_update           = 0x008827b0;

   // ---- Debug / Logging ------------------------------------------------------

   // GameLog(fmt, ...) — printf-style debug logger, __cdecl
   constexpr uintptr_t game_log                    = 0x007E3D50;

   // ---- Debug Drawing (RedCommandConsole / 3D overlay) -----------------------

   constexpr uintptr_t draw_line_3d               = 0x007e96b0;
   constexpr uintptr_t draw_sphere                = 0x007ea240;
   constexpr uintptr_t printf_3d                  = 0x007e9fd0;

   // ---- Physics / Collision ----------------------------------------------------

   constexpr uintptr_t find_body                   = 0x00435830;
   constexpr uintptr_t get_world_xform             = 0x00428a20;
   constexpr uintptr_t get_radius                  = 0x00428260;

   // ---- Debug Console (RedCommandConsole) ------------------------------------

   constexpr uintptr_t console_add_variable        = 0x007ed530;
   constexpr uintptr_t console_add_command         = 0x007ed560;
   constexpr uintptr_t engine_console_reg          = 0x00a145c0; // registers "render_soldier_colliding"

} // namespace modtools

// =============================================================================
// BattlefrontII.exe Steam
// =============================================================================
namespace steam {

   // ---- Lua VM ---------------------------------------------------------------

   constexpr uintptr_t init_state        = 0xDEAD0000; // TODO
   constexpr uintptr_t g_lua_state_ptr   = 0xDEAD0000; // TODO

   constexpr uintptr_t lua_pushcclosure  = 0xDEAD0002; // TODO
   constexpr uintptr_t lua_pushlstring   = 0xDEAD0005; // TODO
   constexpr uintptr_t lua_settable      = 0xDEAD0003; // TODO
   constexpr uintptr_t lua_tolstring     = 0xDEAD0004; // TODO
   constexpr uintptr_t lua_pushnumber    = 0xDEAD0006; // TODO
   constexpr uintptr_t lua_tonumber      = 0xDEAD0007; // TODO
   constexpr uintptr_t lua_gettop        = 0xDEAD0008; // TODO
   constexpr uintptr_t lua_pushnil       = 0xDEAD0009; // TODO
   constexpr uintptr_t lua_pushboolean   = 0xDEAD000A; // TODO
   constexpr uintptr_t lua_toboolean     = 0xDEAD000B; // TODO
   constexpr uintptr_t lua_touserdata    = 0xDEAD000C; // TODO
   constexpr uintptr_t lua_pushlightuserdata = 0xDEAD0013; // TODO
   constexpr uintptr_t lua_isnumber      = 0xDEAD000D; // TODO
   constexpr uintptr_t lua_gettable      = 0xDEAD000E; // TODO
   constexpr uintptr_t lua_pcall         = 0xDEAD000F; // TODO
   constexpr uintptr_t lua_rawgeti       = 0xDEAD0010; // TODO
   constexpr uintptr_t lua_settop        = 0xDEAD0011; // TODO
   constexpr uintptr_t lua_insert        = 0xDEAD0012; // TODO

   // ---- Aimer / Weapon -------------------------------------------------------

   constexpr uintptr_t aimer_set_soldier_info = 0xDEAD0014;                // TODO
   constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0xDEAD0015;  // TODO
   constexpr uintptr_t weapon_override_aimer_impl  = 0xDEAD0016;           // TODO
   constexpr uintptr_t weapon_override_aimer_thunk = 0xDEAD0017;           // TODO

   // ---- Debug / Logging ------------------------------------------------------

   constexpr uintptr_t game_log                    = 0xDEAD0018; // TODO

} // namespace steam

} // namespace game_addrs
