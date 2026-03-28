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

   // ---- Entity / Soldier Prone --------------------------------------------------

   constexpr uintptr_t prone_crouch_inner          = 0x00543B60;
   constexpr uintptr_t prone_standup_inner         = 0x005435D0;
   constexpr uintptr_t prone_set_state             = 0x00406C62;
   constexpr uintptr_t prone_get_foley_fx          = 0x0040E1DD;
   constexpr uintptr_t prone_game_sound_play       = 0x00415451;
   constexpr uintptr_t prone_anim_accessor         = 0x005701F0;
   constexpr uintptr_t prone_set_action            = 0x00575D50;
   constexpr uintptr_t prone_vtable_slot           = 0x00A40718;
   constexpr uintptr_t prone_guard_jnz             = 0x00545BA6;
   constexpr uintptr_t prone_acklay_gate_jnz       = 0x0052C28E;
   constexpr uintptr_t prone_height_jump_table     = 0x0053C000;
   constexpr uintptr_t prone_height_switch_end     = 0x0053BD67;
   constexpr uintptr_t prone_primary_stance_and    = 0x005C4506;

   // ---- Entity / Cloth ---------------------------------------------------------

   constexpr uintptr_t cloth_satisfy_constraints    = 0x004cae40;
   constexpr uintptr_t cloth_enforce_collisions     = 0x004cabd0;
   constexpr uintptr_t cloth_enforce_cylinder_coll  = 0x004c8660;

   // ---- Animation ---------------------------------------------------------------

   constexpr uintptr_t fp_anim_set_property        = 0x0053FA20;
   constexpr uintptr_t fp_update_soldier            = 0x004A9BE0;
   constexpr uintptr_t anim_add_bank               = 0x004A8FC0;
   constexpr uintptr_t anim_find_animation         = 0x004A7900;
   constexpr uintptr_t fp_anim_array               = 0x00B70E30;  // ZephyrAnim*[48]
   constexpr uintptr_t anim_name_table             = 0x00A36C88;  // const char*[48]

   // ---- Weapon / Disguise ------------------------------------------------------

   constexpr uintptr_t disguise_set_property       = 0x0062A320;
   constexpr uintptr_t disguise_raise              = 0x0062AAD0;
   constexpr uintptr_t disguise_drop               = 0x0062A180;
   constexpr uintptr_t game_model_table            = 0x00B76CC4;

   // ---- Character System --------------------------------------------------------

   constexpr uintptr_t char_array_base              = 0xB93A08;
   constexpr uintptr_t max_chars                    = 0xB939F4;
   constexpr uintptr_t team_array_base              = 0xAD5D64;
   constexpr uintptr_t class_def_list               = 0xACD2C8;

   // ---- Animation (weapon/soldier) ---------------------------------------------

   constexpr uintptr_t get_weapon_anim_map          = 0x00570760;
   constexpr uintptr_t set_weapon_anim_map          = 0x004170D5;
   constexpr uintptr_t assign_animations            = 0x00581AF0;
   constexpr uintptr_t anim_instance                = 0x00B8D3C4;

   // ---- Entity / Vehicle -------------------------------------------------------

   constexpr uintptr_t char_exit_vehicle            = 0x0052FC70;

   // ---- Entity / Vehicle (Carrier/Flyer) ---------------------------------------

   constexpr uintptr_t carrier_set_property         = 0x004D7210;
   constexpr uintptr_t carrier_attach_cargo         = 0x004D81F0;
   constexpr uintptr_t carrier_detach_cargo         = 0x004D8350;
   constexpr uintptr_t carrier_initiate_landing     = 0x004f1380;
   constexpr uintptr_t carrier_kill                 = 0x004D8400;
   constexpr uintptr_t carrier_update               = 0x004D7FE0;
   constexpr uintptr_t carrier_update_landed_ht     = 0x004D8130;
   constexpr uintptr_t carrier_update_spawn         = 0x00665A50;
   constexpr uintptr_t carrier_take_off             = 0x004F8B70;
   constexpr uintptr_t carrier_vtable               = 0x00A3A670;
   constexpr uintptr_t flyer_render                 = 0x004f6970;
   constexpr uintptr_t turret_update_indirect       = 0x005673a0;
   constexpr uintptr_t turret_activate              = 0x00563a90;
   constexpr uintptr_t aimer_activate               = 0x005ef020;
   constexpr uintptr_t passenger_activate           = 0x00568540;
   constexpr uintptr_t mem_pool_alloc               = 0x00802300;
   constexpr uintptr_t vehicle_tracker_pool         = 0x00B9A758;

   // Carrier inline patch sites
   constexpr uintptr_t turret_fire_check            = 0x00565c4c;
   constexpr uintptr_t turret_fire_allow            = 0x00565c5d;
   constexpr uintptr_t turret_fire_block            = 0x00565c83;
   constexpr uintptr_t create_ctrl_patch            = 0x0055b2e8;
   constexpr uintptr_t create_ctrl_resume           = 0x0055b359;
   constexpr uintptr_t player_ctrl_ctor             = 0x0040d1e8;

   // ---- Loading Screen (extended) ----------------------------------------------

   constexpr uintptr_t enter_state_path_op          = 0x0067e388;

   // ---- Hashing (thiscall wrapper) ---------------------------------------------

   constexpr uintptr_t hash_string_thiscall         = 0x007E1BD0;

   // ---- Shell / GC Visual Limits -----------------------------------------------

   constexpr uintptr_t gc_beam_add                  = 0x0045A920;
   constexpr uintptr_t gc_particle_add              = 0x0045A9E0;

   constexpr uintptr_t gc_beam_count_patches[]      = {
       0x0045A922, 0x0045A938,
       0x0045ADC8, 0x0045B28D, 0x0045B2A8, 0x0045B2BC,
       0x0045B924,
   };

   constexpr uintptr_t gc_particle_count_patches[]  = {
       0x0045A9E2, 0x0045A9FE,
       0x0045B629, 0x0045B6C0, 0x0045B6D7, 0x0045B6E9,
       0x0045B8E5,
   };

   constexpr uintptr_t gc_particle_alloc_size_op    = 0x0045B8BD;
   constexpr uintptr_t gc_beam_alloc_size_op        = 0x0045B8FD;

   // ---- State Machine / Triggers -----------------------------------------------

   constexpr uintptr_t trigger_update              = 0x00562dd0;

   // ---- Physics / Body Management (extended) ---------------------------------

   constexpr uintptr_t remove_body                 = 0x0042ac60;
   constexpr uintptr_t add_item_body               = 0x0042dd00;
   constexpr uintptr_t vec_scale                   = 0x004294b0;

   // ---- Weapon / Grappling Hook -----------------------------------------------

   constexpr uintptr_t grapple_update              = 0x0060f380;
   constexpr uintptr_t grapple_dtor                = 0x0060ef90;
   constexpr uintptr_t grapple_check_fire          = 0x0062c760;
   constexpr uintptr_t grapple_ord_render          = 0x0060fb80;
   constexpr uintptr_t grapple_set_property        = 0x0060EC60;
   constexpr uintptr_t grapple_set_visibility      = 0x005297b0;
   constexpr uintptr_t grapple_rtti_hash           = 0x00b7e098;
   constexpr uintptr_t grapple_rso_vtable          = 0x00A50E98;

   // ---- Spline / Cable Rendering ----------------------------------------------

   constexpr uintptr_t spline_build                = 0x0083e720;
   constexpr uintptr_t cable_render                = 0x006d2370;

   // ---- Debug / Visualization --------------------------------------------------

   constexpr uintptr_t hover_post_coll_update       = 0x00514490;
   constexpr uintptr_t freecam_update               = 0x004ae1b0;
   constexpr uintptr_t soldier_pcu                   = 0x00530B20;

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
