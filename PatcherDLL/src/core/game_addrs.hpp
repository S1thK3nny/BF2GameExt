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

   // Stock Lua callback: CreateEntity(class, matrix, name). Detoured to apply
   // VehicleSpawn-style post-create fixup (team + activate) so vehicles
   // spawned this way can actually fire weapons.
   constexpr uintptr_t lua_create_entity = 0x00472730;

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

   // ---- Terrain RedTexture cleanup (port of upstream 4a8d0df) -----------------
   // ReadTerrain (__cdecl(void* reader)); detoured to re-resolve the terrain
   // shader's cached detail/white RedTexture* each map load (fixes stale
   // pointers when a playlist map lacks a detail map).
   constexpr uintptr_t read_terrain              = 0x007e5e10;
   constexpr uintptr_t terrain_null_detail_texture = 0x00edd114; // RedTexture**
   constexpr uintptr_t terrain_white_texture       = 0x00edd11c; // RedTexture**

   // ---- BlurEffect downsize clamp (port of upstream 1f8f618) -------------------
   // BlurEffect::Render body (__thiscall(self, uint flags); the vtable slot holds
   // the ILT thunk 0x00409755 -> this body).  Detoured to clamp the downsized
   // render-target resolution (mDownsizeFactor @ +0x30) to 512px max.
   constexpr uintptr_t blur_effect_render              = 0x0077a930;
   // RedRenderer::pcGetViewportExtents(__cdecl float* minX, minY, maxX, maxY)
   constexpr uintptr_t red_renderer_get_viewport_extents = 0x00805f40;

   // ---- GameState (DLC mission-list init fix, port of upstream e8b6fa7) --------
   // Static GameState::State objects/functions.  vtable: [0]=dtor [1]=Enter
   // [2]=Update [3]=Exit.  shell_state is the OBJECT address, not a pointer.
   constexpr uintptr_t gamestate_shell_state           = 0x00ac6d84;
   constexpr uintptr_t gamestate_shell_state_enter     = 0x00407ac7; // ILT thunk -> ShellState::Enter
   constexpr uintptr_t gamestate_mission_state_enter   = 0x00407fb8; // ILT thunk -> MissionState::Enter

   // ---- EntityHover self-piloted crash fix -----------------------------------
   // EntityHover::UpdateIndirect (0x515cc0) AI obstacle-avoidance fetches the
   // hover's pilot and calls pilot->GetGameObject() with no null check; a
   // self-piloted hover (PilotType=self) has no pilot -> the getter returns
   // null -> AV read [pilot+0x18] at 0x515e3e (SP and MP).
   //   ..._pilot_call : the `E8 rel32` CALL to the getter, immediately followed
   //                    by `8B 50 18 8D 48 18 FF 52 20` (the unguarded deref).
   //   ..._get_active_pilot : the getter (FUN_004d49f0) -> mPilot, or null when
   //                    PilotType is self / vehicleself-on-self.
   constexpr uintptr_t hover_updateindirect_pilot_call = 0x00515e39;
   constexpr uintptr_t controllable_get_active_pilot   = 0x004d49f0;

   // Second self-piloted-hover crash: issuing a unit order crashes in
   // EntitySoldier::Update's event-0x1a/0x1b order-acknowledge block, which
   // derefs the same null pilot link (+0xCC) on a self-piloted hover.
   //   crash_site  : `8B BF 48 01 00 00` MOV EDI,[EDI+0x148] (EDI = null pilot).
   //   skip_target : the block's convergence point (also the GetNumCameras()==0
   //                 branch target), reached at block-level ESP.
   constexpr uintptr_t hover_command_crash_site  = 0x00549bf8;
   constexpr uintptr_t hover_command_skip_target = 0x00549cfb;

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

   // GameSoundEngine::gEnableSoundWarnings — bool gating the "GameSound::SetID -
   // Unable to find sound property" RedWarning (and the sibling "not loaded"
   // warning) in GameSound::SetID.  Read-only at runtime (the game never writes
   // it); image default is 0 (warnings off).  Set to 1 to surface missing-sound
   // warnings.  Retail (Steam/GOG) compiled this warning code out entirely — the
   // sole GameSound::SetID has no such read and the strings are stripped — so the
   // feature is modtools-only.
   constexpr uintptr_t g_enable_sound_warnings     = 0x00cf41d8;

   // Snd::PrintDebugString(char*) — the Snd::Globals warning function that every
   // sound warning routes through (via Snd::PrintDebugMessage).  It wraps the
   // message as "Sound (%s)" for RedWarning + also PblTraceP's it.  The upstream
   // Snd::PrintDebugMessage always appends a '\n' on top of format strings that
   // already end in one, so the "%s" injects "\n\n" *inside* the parens (blank
   // line + orphan ')').  Detoured to strip trailing newlines before the wrap.
   constexpr uintptr_t snd_print_debug_string      = 0x0074e6f0;

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

   constexpr uintptr_t EntitySoldier_crouch        = 0x00543B60;
   constexpr uintptr_t EntitySoldier_stand         = 0x005435D0;
   constexpr uintptr_t EntitySoldier_prone         = 0x00A40718;
   constexpr uintptr_t EntitySoldier_SetState             = 0x00406C62;
   constexpr uintptr_t FoleyFXCollider_GetFoleyFX  = 0x0040E1DD;
   constexpr uintptr_t GameSound_play              = 0x00415451;
   constexpr uintptr_t prone_anim_accessor         = 0x005701F0;
   constexpr uintptr_t SoldierAnimator_SetAction   = 0x00575D50;
   constexpr uintptr_t prone_guard_jnz             = 0x00545BA6;
   constexpr uintptr_t prone_acklay_gate_jnz       = 0x0052C28E;
   constexpr uintptr_t prone_height_jump_table     = 0x0053C000;
   constexpr uintptr_t prone_height_switch_end     = 0x0053BD67;
   constexpr uintptr_t prone_primary_stance_and    = 0x005C4506;
   constexpr uintptr_t WeaponMeleeClass_vftable   = 0x00A5434C;

   constexpr uintptr_t lua_read_data_file          = 0x0046A790;
   constexpr uintptr_t load_util_read_data_file    = 0x004538B0;

   constexpr uintptr_t lowres_postload             = 0x00586E60; // SoldierAnimatorLowResClass::PostLoad
   constexpr uintptr_t pbl_hash_table_store        = 0x007E1A90; // PblHashTableCode::_Store(table, size, hash, value)
   constexpr uintptr_t pbl_temp_hash               = 0x007E1C10; // PblTEMPHash

   // ---- Entity / Droideka DisableBallMode --------------------------------------
   // EntityDroideka::UpdatePilot is the sole roll/unroll request site (player and
   // AI both).  Struct offsets (mClass +0x450, mState +0x1A74) live with the
   // build switch in entity/droideka_ball_mode.cpp.

   constexpr uintptr_t droideka_class_set_property = 0x004EA800; // class vtable 0xa3bb00 slot 6
   constexpr uintptr_t droideka_update_pilot       = 0x004E8250; // entity vtable +0x120
   constexpr uintptr_t droideka_class_derive       = 0x004E5400; // class vtable 0xa3bb00 slot 1

   // ---- Entity / Droideka death animation fix ---------------------------------
   // The `CALL [EDX+0x130]` (NextState) inside EntityDroideka::Update (0x4ee5a0)
   // that re-issues the die input every frame while mIsDead.  ECX = entity base
   // at the call.  Guards above it: TEST [ESI+0x1aa4],8 @0x4ef23d (mIsDead),
   // CMP [ESI+0x1a74],4 @0x4ef24a (mState==dead).

   constexpr uintptr_t droideka_update_nextstate_call = 0x004EF2FA;

   // ---- Entity / Soldier override textures (OverrideTexture3..5) ---------------
   // See entity/soldier_override_texture.cpp. Extends the stock 2-slot soldier
   // override_texture system to 5. Stock reads two class fields (+0x988/+0x98C)
   // in the render funcs below and binds them, via RedShadingPose::CreateShadingState
   // keyed by the model material-name hash, to shader int-param 0x891891e9. We add
   // slots 3-5 from a class side table. hash_string (above) = PblHash::_MakeHash.
   constexpr uintptr_t soldier_class_set_property  = 0x0053FA20; // EntitySoldierClass::SetProperty
   constexpr uintptr_t soldier_render              = 0x00535D90; // EntitySoldier::Render (this->class @ +0x3C4)
   constexpr uintptr_t soldier_element_render_ctx  = 0x00674890; // SoldierElement::RenderUsingContext (this->class @ +0x130)
   constexpr uintptr_t shading_pose_create_state   = 0x0083E4E0; // RedShadingPose::CreateShadingState(pose, matNameHash)
   constexpr uintptr_t shading_state_get_int_param = 0x0083E1B0; // RedShadingState::GetIntParam(state, paramHash)

   // ---- Entity / Cloth ---------------------------------------------------------

   constexpr uintptr_t cloth_satisfy_constraints    = 0x004cae40;
   constexpr uintptr_t cloth_enforce_collisions     = 0x004cabd0;
   constexpr uintptr_t cloth_enforce_cylinder_coll  = 0x004c8660;

   // ---- Animation ---------------------------------------------------------------

   constexpr uintptr_t fp_update_soldier            = 0x004A9BE0;
   constexpr uintptr_t anim_add_bank               = 0x004A8FC0;
   constexpr uintptr_t anim_find_animation         = 0x004A7900;
   constexpr uintptr_t fp_anim_array               = 0x00B70E30;  // ZephyrAnim*[48]
   constexpr uintptr_t anim_name_table             = 0x00A36C88;  // const char*[48]
   constexpr uintptr_t fp_renderable               = 0x00B70F40;  // FirstPerson::s_pRenderable — FirstPersonRenderable* (size-1 array on PC, one per camera); +0x1600 = mCurrentWeapon

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
   constexpr uintptr_t aimer_set_weapon             = 0x00407B76;  // Aimer::SetWeapon(Weapon*) ILT thunk — ECX=Aimer*, called by EntitySoldier ctor @0x533ffc

   // ---- Animation (weapon/soldier) ---------------------------------------------

   constexpr uintptr_t get_weapon_anim_map          = 0x00570760;
   constexpr uintptr_t set_weapon_anim_map          = 0x004170D5;
   constexpr uintptr_t anim_finder_add_bank        = 0x00580860;  // AnimationFinder::_AddBank
   constexpr uintptr_t anim_finder_add_entry       = 0x0057E220;  // AnimationFinder: add RedAnimation* to bank array
   constexpr uintptr_t anim_add_skeleton_bank      = 0x0057DEC0;  // FUN_0057dec0: skeleton-shared bank add (line 671, writes inline mAnimBankOld directly)
   constexpr uintptr_t anim_finder_resolve         = 0x0057F860;  // FUN_0057f860: AnimationFinder resolve loop (reads finder->mAnimBank, calls RedAnimation::FindAnimation)
   constexpr uintptr_t anim_class_find_in_banks    = 0x0057DE40;  // SoldierAnimatorClass::FindAnimation — iterates this->mAnimBankOld[0..count] inline, calls RedAnimation::FindAnimation
   constexpr uintptr_t red_find_animation          = 0x008037B0;  // RedAnimation::FindAnimation(hash, name)
   constexpr uintptr_t anim_hash_table             = 0x00D5B9E4;  // global PblHashTableCode for RedAnimation

   // ---- Entity / Vehicle -------------------------------------------------------

   constexpr uintptr_t char_exit_vehicle            = 0x0052FC70;

   // ---- Entity / Vehicle (Carrier/Flyer) ---------------------------------------

   constexpr uintptr_t flyer_init_animations         = 0x004F6560;
   constexpr uintptr_t zephyr_anim_bank_find        = 0x00803750;

   // ---- Animation / ZephyrPose (skeletal animation evaluation) ---------------

   constexpr uintptr_t zephyr_pose_dyn_set_anim     = 0x0082AAC0;
   constexpr uintptr_t zephyr_pose_dyn_set_time     = 0x0082A9C0;
   constexpr uintptr_t zephyr_pose_static_ctor      = 0x0082C9D0;
   constexpr uintptr_t zephyr_pose_static_dtor      = 0x0082CA00;
   constexpr uintptr_t zephyr_pose_static_open      = 0x0082CA10;
   constexpr uintptr_t zephyr_pose_static_set       = 0x0082D370;
   constexpr uintptr_t zephyr_pose_static_blend     = 0x0082D580;
   constexpr uintptr_t zephyr_skeleton_open         = 0x0082B660;
   constexpr uintptr_t zephyr_skeleton_finalize     = 0x0082C390;
   constexpr uintptr_t red_pose_convert_skel32      = 0x0082DA80;
   constexpr uintptr_t g_identity_matrix            = 0x00CF6830;
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

   // ---- Flyer path following / engine sound --------------------------------
   constexpr uintptr_t path_follower_land_jg        = 0x005ED340;  // EntityPathFollower::Update — JG gating the LandOnArrival check to node 0 (7F 25)
   constexpr uintptr_t entity_flyer_land            = 0x004F1380;  // EntityFlyer::Land (== carrier_initiate_landing)
   constexpr uintptr_t path_follower_reset          = 0x005E7290;  // EntityPathFollower::Reset(pathClass) — __thiscall, RET 4
   constexpr uintptr_t vehicle_engine_update        = 0x007600F0;  // VehicleEngine::Update — __thiscall, 14 stack args, RET 0x38
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

   // ---- RedParticleRenderer (batching caches used by the GC galaxy map) ---------
   // SubmitParticle(type, pos, colorPtr, size, u, rgba, vec3, vec3) — __cdecl,
   // 8 stack dwords. currentCache/cacheIndex/caches: 15 caches of 0x3558 bytes,
   // 200 particle entries (0x44 bytes) each; header fields at +0x3520.
   constexpr uintptr_t rpr_submit_particle          = 0x00825180;
   constexpr uintptr_t rpr_current_cache            = 0x00E5F644;
   constexpr uintptr_t rpr_cache_index              = 0x00E5F648;
   // SetCurrentCache found-entry path "ADD ECX, s_caches" imm32 operand — holds
   // the LIVE array base (the DLL's 120-slot g_sCaches_storage when the
   // Particle Cache Increase redirect is applied, else the exe's s_caches[15]).
   constexpr uintptr_t rpr_setcache_base_operand    = 0x00824D55;
   // SetCurrentCache allocation clamp "CMP EDX, 0xF" imm8 operand.
   constexpr uintptr_t rpr_setcache_limit_imm8_op   = 0x00824D3D;

   // ---- GameLoop state ---------------------------------------------------------

   constexpr uintptr_t gameloop_pause_mode          = 0x00c6aae8;  // bool, true when ESC paused

   // ---- Low-res animation table ------------------------------------------------

   // Pointer entry in the lowres animation name table for prone (index 2).
   // Points to "rifle_crouch_idle_takeknee" by default — patch to use prone anim.
   constexpr uintptr_t lowres_prone_anim_name_ptr   = 0x00acfa68;

   // Jump table entry in GetAnimatorLocal_ for prone (case 2).
   // Points to ESI=1 (crouch idle) — patch to point to ESI=2 (prone anim).
   constexpr uintptr_t lowres_prone_jump_entry      = 0x005886a8;
   constexpr uintptr_t lowres_prone_jump_target     = 0x00588575;  // MOV ESI,2

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

   // ---- Weapon / Shield ---------------------------------------------------------

   constexpr uintptr_t weapon_update                = 0x0061D850;  // Weapon::Update
   constexpr uintptr_t weapon_shield_update         = 0x0063F360;  // WeaponShield::Update

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

   // ---- Particle / Renderer Cache (BSS globals) --------------------------------

   constexpr uintptr_t s_cached_particles            = 0x00B9DB78;  // sCachedParticles[300]
   constexpr uintptr_t s_caches                      = 0x00E5F650;  // RedParticleRenderer s_caches[15]

   // ---- Controller / Input -------------------------------------------------------

   constexpr uintptr_t controller_base_global   = 0x00CAEB20;
   constexpr uintptr_t num_joysticks_global     = 0x00D2BDA8;
   constexpr uintptr_t joystick_config_base     = 0x00CB2A78;
   constexpr uintptr_t joystick_discover        = 0x007485F0;
   constexpr uintptr_t joystick_sync            = 0x007489A0;

   // ---- Rumble -------------------------------------------------------------------

   constexpr uintptr_t rumble_light_output      = 0x0084FF00;
   constexpr uintptr_t rumble_heavy_output      = 0x0084FEF0;
   constexpr uintptr_t rumble_state_setup       = 0x007413A0;
   constexpr uintptr_t s_game_over              = 0x00C6AAE0;
   constexpr uintptr_t weapon_signal_fire       = 0x0061C870;

   // ---- Aim Assist ---------------------------------------------------------------

   constexpr uintptr_t player_controller_update       = 0x0059B460;
   constexpr uintptr_t apply_damage                   = 0x004CF900;
   constexpr uintptr_t lockon_mgr_array               = 0x00B30698;
   constexpr uintptr_t get_cur_wpn                    = 0x00413782;
   constexpr uintptr_t set_target_locked_obj          = 0x00411013;  // thunk -> 0x005e0d30
   constexpr uintptr_t m_camera_global                = 0x00d61ddc;
   constexpr uintptr_t team_get_objects_in_range      = 0x0048f210;

   // ---- Vehicle view toggle (FP/TP) -------------------------------------------

   // EntityHover / EntityWalker (and their network-replicated CommandHover /
   // CommandWalker counterparts) share a Controllable-aimer subobject at
   // struct_base + 0x258 whose vtable slot +0x3C is a const-true stub.  That
   // slot is queried by the change-view gate in each vehicle's Update; const
   // true means the gate always suppresses the toggle, so ground vehicles
   // can't switch FP/TP unless ForceMode is set in the ODF.  Repoint the
   // slot at the existing const-false thunk (used by the same vtables'
   // slot +0x40) so the gate falls through to the toggle call.
   constexpr uintptr_t veh_view_hover_vtable_3c_slot     = 0x00A3DDA4;
   constexpr uintptr_t veh_view_walker_vtable_3c_slot    = 0x00A41EB4;
   constexpr uintptr_t veh_view_cmd_hover_vtable_3c_slot  = 0x00A5709C;
   constexpr uintptr_t veh_view_cmd_walker_vtable_3c_slot = 0x00A57CAC;
   constexpr uintptr_t veh_view_hover_3c_orig_thunk      = 0x004139E9; // -> 0x005124E0 (return 1)
   constexpr uintptr_t veh_view_walker_3c_orig_thunk     = 0x004127CE; // -> 0x00551B10 (return 1)
   constexpr uintptr_t veh_view_return_false_thunk       = 0x00415A50; // -> 0x004BAAE0 (return 0)

   // ---- Networking state (multiplayer detection) ---------------------------------

   constexpr uintptr_t net_in_shell                   = 0x00ADABC2;
   constexpr uintptr_t net_enabled                    = 0x00BE14F0;
   constexpr uintptr_t net_enabled_next               = 0x00BE14F1;
   constexpr uintptr_t net_on_client                  = 0x00BE14FD;

   // ---- Fog (SetFogRange / SetFogEnable Lua funcs) ------------------------------
   // RedRenderer::SetFogRange/SetFogEnable set the D3D render states; the
   // FLRenderer m_fFogStart/m_fFogEnd globals persist the range (FLRenderer::
   // SetFogRange only stores these two dwords, so the Lua func writes them
   // directly — release builds inlined the setter away).
   constexpr uintptr_t red_renderer_set_fog_range     = 0x0080B920;
   constexpr uintptr_t red_renderer_set_fog_enable    = 0x0080B900;
   constexpr uintptr_t fl_fog_start                   = 0x00E5BA64;
   constexpr uintptr_t fl_fog_end                     = 0x00E5BB4C;

   // ---- Weapon / Animated Lightsaber Textures (anim_textures.cpp) --------------
   // Port of Xbox's AnimTexture1-3 WeaponMelee blade properties (4-frame blade
   // texture cycle).  _RenderLightsabre is pure __cdecl (7 stack args) on
   // modtools; the blade array lives at WeaponMeleeClass+0x3DC (entries 0x34
   // bytes, base texture hash at +0x28).  The "Blade" parse path stashes the
   // model / name-hash / unknown into these globals for BladeLookup.
   constexpr uintptr_t lightsabre_render            = 0x00633660;
   constexpr uintptr_t weaponmelee_set_property     = 0x0063AB40;
   constexpr uintptr_t blade_lookup                 = 0x00635180;
   constexpr uintptr_t blade_global_model           = 0x00B92AD8;
   constexpr uintptr_t blade_global_namehash        = 0x00B92AD4;
   constexpr uintptr_t blade_global_unk             = 0x00B92AD0;

   // ---- HUD Widescreen Reticle Correction (hud_widescreen.cpp) -----------------
   // Pre-distorts the reticle Y in ReticuleDisplay::Update so it lands on the
   // correct 3D aim point after the vanilla letterbox transform.  Modtools uses
   // x87: redirect the FADD [1.0] / FMUL [0.5] constant-address operands to
   // DLL-controlled statics.  ReticuleDisplay::Update (which owns the reticle
   // transform we patch) is detoured to refresh the correction params from the
   // live screen dimensions each frame.
   constexpr uintptr_t reticle_display_update       = 0x00683270; // ReticuleDisplay::Update __thiscall bool(this,float)
   constexpr uintptr_t hud_screen_width             = 0x00E5B508;
   constexpr uintptr_t hud_screen_height            = 0x00E5B50C;
   constexpr uintptr_t hud_reticle_fadd_operand     = 0x006834D9; // FADD [1.0] operand at +2
   constexpr uintptr_t hud_reticle_fmul_operand     = 0x006834E5; // FMUL [0.5] operand at +2

} // namespace modtools

// =============================================================================
// BattlefrontII.exe Steam
// =============================================================================
namespace steam {

   // ---- Lua VM ---------------------------------------------------------------

   constexpr uintptr_t init_state        = 0x5a0ce0;
   constexpr uintptr_t g_lua_state_ptr   = 0x1e579a0;

   constexpr uintptr_t lua_pushcclosure  = 0x69bf30;
   constexpr uintptr_t lua_pushlstring   = 0x69c000;
   constexpr uintptr_t lua_settable      = 0x69c3d0;
   constexpr uintptr_t lua_tolstring     = 0x69c5b0;
   constexpr uintptr_t lua_pushnumber    = 0x69c060;
   constexpr uintptr_t lua_tonumber      = 0x69c510;
   constexpr uintptr_t lua_gettop        = 0x69bba0;
   constexpr uintptr_t lua_pushnil       = 0x69c040;
   constexpr uintptr_t lua_pushboolean   = 0x69bf10;
   constexpr uintptr_t lua_toboolean     = 0x69c4d0;
   constexpr uintptr_t lua_touserdata    = 0x69c640;
   constexpr uintptr_t lua_pushlightuserdata = 0x69bfe0;
   constexpr uintptr_t lua_isnumber      = 0x69bc80;
   constexpr uintptr_t lua_gettable      = 0x69bb60;
   constexpr uintptr_t lua_pcall         = 0x69beb0;
   constexpr uintptr_t lua_rawgeti       = 0x69c1a0;
   constexpr uintptr_t lua_settop        = 0x69c400;
   constexpr uintptr_t lua_insert        = 0x69bc00;

   // ---- Aimer / Weapon -------------------------------------------------------

   constexpr uintptr_t aimer_set_soldier_info = 0x0043d290;
   constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0x007b05ec; // WeaponCannon vftable (0x7b057c) + slot 28*4
   constexpr uintptr_t weapon_override_aimer_impl  = 0x00677780;          // Weapon::OverrideAimer (default `return 0`)
   constexpr uintptr_t weapon_override_aimer_thunk = 0x00677780;          // No ILT thunk in release build; same as impl
   constexpr uintptr_t weapon_zoom_first_person = 0x00677d40;
   constexpr uintptr_t weapon_update            = 0x006781B0;             // Weapon vtable (0x7b01a8) slot 1
   // WeaponShield ctor (Steam 0x006917c0) does NOT write its own vtable — inherits Weapon's.
   // weapon_shield_update therefore aliases weapon_update in Steam. Verify behavior before use.
   constexpr uintptr_t weapon_shield_update     = 0x006781B0;             // Aliases weapon_update (no override in Steam build)

   // ---- Hashing / Texture lookup ---------------------------------------------

   constexpr uintptr_t hash_string               = 0x00726e50;  // PblHash::calcHash (__cdecl)
   constexpr uintptr_t pbl_hash_table_find       = 0x00726e00;  // PblHashTableCode::_Find
   constexpr uintptr_t tex_hash_table            = 0x008eed8c;  // RedTexture PblHashTable

   // ---- Terrain RedTexture cleanup (port of upstream 4a8d0df) -----------------
   constexpr uintptr_t read_terrain              = 0x006c2460;
   constexpr uintptr_t terrain_null_detail_texture = 0x009c922c;
   constexpr uintptr_t terrain_white_texture       = 0x009c9230;

   // ---- BlurEffect downsize clamp (port of upstream 1f8f618) -------------------
   constexpr uintptr_t blur_effect_render              = 0x0040f8d0;
   constexpr uintptr_t red_renderer_get_viewport_extents = 0x006b86a0;

   // ---- Screenshot redirect (port of upstream 9a6d4b9) -------------------------
   // The game's IDirect3DDevice9* global.  Careful: under Shader Patch the wrong
   // call through this could crash — we only ever do backbuffer copy + lock.
   constexpr uintptr_t d3d_device                      = 0x007f594c;
   // E8 rel32 call site of the broken Screenshot::RequestScreenshot (target is
   // read from the rel32 at install time and detoured).
   constexpr uintptr_t screenshot_request_call_site    = 0x00533520;

   // ---- RedWarning::DialogBoxMessage fix (port of upstream aefa406) ------------
   // FF 15 [DialogBoxParamA] indirect call inside RedWarning::DialogBoxMessage.
   // The retail exes lack the dialog template resource, so the call always fails;
   // rewritten to 90 E8 rel32 -> our shim using a template in BF2GameExt.dll.
   constexpr uintptr_t red_warning_dialog_call_site    = 0x006f6b69;

   // ---- GameState (DLC mission-list init fix, port of upstream e8b6fa7) --------
   constexpr uintptr_t gamestate_shell_state           = 0x007eb998;
   constexpr uintptr_t gamestate_shell_state_enter     = 0x0053b980;
   constexpr uintptr_t gamestate_mission_state_enter   = 0x0053bae0;

   // ---- EntityHover self-piloted crash fix (see modtools notes) --------------
   // Steam EntityHover::UpdateIndirect is vfunction50 @0x4c6ef0; crash at
   // 0x4c703e.  The compiler emitted a different (but equivalent) GetGameObject
   // idiom here — `8D 48 18  8B 01  8B 40 20  FF D0` — see the per-build guard
   // in hover_pilot_null_fix.cpp.  Getter (FUN_0043aad0) is byte-identical to
   // modtools' (reads +0x144 pilotType / +0xd0 mPilot).
   constexpr uintptr_t hover_updateindirect_pilot_call = 0x004c7036;
   constexpr uintptr_t controllable_get_active_pilot   = 0x0043aad0;

   // Second self-piloted-hover crash (unit-order path).  Steam codegen differs:
   // the null link is in EAX and the crash instr is `MOV ECX,[EAX+0x148]`
   // (8B 88 48 01 00 00) preceded by `MOV EAX,[EAX+0xCC]` (8B 80 CC..).  Unlike
   // modtools there is NO pending stack here (the preceding call's args are
   // cleaned before the chain), so the skip is a bare jump to the convergence
   // point (the GetNumCameras()==0 branch target, reached at block-level ESP).
   constexpr uintptr_t hover_command_crash_site  = 0x004ece30;
   constexpr uintptr_t hover_command_skip_target = 0x004ecb1e;

   // ---- Memory heap management -----------------------------------------------

   constexpr uintptr_t red_set_current_heap      = 0x006C3C10;  // RedSetCurrentHeap

   // ---- Entity / Soldier Prone -----------------------------------------------
   constexpr uintptr_t EntitySoldier_prone       = 0x0079cfcc;
   constexpr uintptr_t EntitySoldier_crouch      = 0x004ed550;
   constexpr uintptr_t EntitySoldier_stand       = 0x004ed080;
   constexpr uintptr_t EntitySoldier_SetState    = 0x004ee2c0;
   constexpr uintptr_t FoleyFXCollider_GetFoleyFX = 0x004e7bf0;
   constexpr uintptr_t prone_anim_accessor       = 0x0063c2d0;
   constexpr uintptr_t SoldierAnimator_SetAction = 0x0063ed60;
   constexpr uintptr_t prone_guard_jnz           = 0x004e8968;
   constexpr uintptr_t prone_acklay_gate_jnz     = 0x004e67c0;
   constexpr uintptr_t prone_height_jump_table   = 0x004F07BC;
   constexpr uintptr_t prone_height_switch_end   = 0x004F04F3;
   constexpr uintptr_t prone_primary_stance_and  = 0x005435E4;
   constexpr uintptr_t WeaponMeleeClass_vftable  = 0x007B1534;

   constexpr uintptr_t lua_read_data_file        = 0x0058AC50;
   constexpr uintptr_t load_util_read_data_file  = 0x00579C30;

   constexpr uintptr_t lowres_postload           = 0x00647D40;
   constexpr uintptr_t pbl_hash_table_store      = 0x00726F60;  // returns bool here
   constexpr uintptr_t pbl_temp_hash             = 0x00726D80;

   // ---- Entity / Droideka DisableBallMode ------------------------------------
   // Release layout differs from modtools (mState +0x1A54 not 0x1A74, mClass
   // +0x438 not 0x450) — offsets live with the build switch in
   // entity/droideka_ball_mode.cpp.

   constexpr uintptr_t droideka_class_set_property = 0x004A82A0; // class vtable 0x79aeb4 slot 6
   constexpr uintptr_t droideka_update_pilot       = 0x004A2030; // entity vtable +0x120
   constexpr uintptr_t droideka_class_derive       = 0x004A8180; // class vtable 0x79aeb4 slot 1

   // ---- Entity / Soldier override textures (OverrideTexture3..5) --------------
   // Steam offsets differ from modtools (release vs debug). Class fields the stock
   // render binds are +0x794/+0x798. See entity/soldier_override_texture.cpp.
   constexpr uintptr_t soldier_class_set_property  = 0x004F82E0; // EntitySoldierClass::SetProperty
   constexpr uintptr_t soldier_render              = 0x004E23D0; // EntitySoldier::Render (this->class @ +0x3AC)
   constexpr uintptr_t soldier_element_render_ctx  = 0x0048DC90; // SoldierElement::RenderUsingContext (this->class @ +0x130)
   constexpr uintptr_t shading_pose_create_state   = 0x006ED4F0; // RedShadingPose::CreateShadingState
   constexpr uintptr_t shading_state_get_int_param = 0x006ED5F0; // RedShadingState::GetIntParam

   // ---- Entity / Droideka death animation fix --------------------------------
   // The `CALL [EAX+0x130]` (NextState) inside EntityDroideka::Update that
   // re-issues the die input every frame while mIsDead.  ECX = entity base at
   // the call.  Guard above it: CMP [EBX+0x1a54],4 @0x4a4ae0 (mState==dead).

   constexpr uintptr_t droideka_update_nextstate_call = 0x004A4B94;

   // Lowres name table entry [2] ("rifle_crouch_idle_takeknee" @ 0x7AE7F8);
   // 8-byte entries {name*, flag} based 0x7E99D0.
   constexpr uintptr_t lowres_prone_anim_name_ptr = 0x007E99E0;
   // GetAnimatorLocal (0x648ff0) dispatch: byte map @0x649368 routes state 2
   // (PRONE) to its own dedicated case @0x6491C9 = "MOV EBX,1; JMP merge"
   // (EBX = lowres pose index -> crouch pose).  Patch the imm byte 1 -> 2 so
   // prone uses pose slot 2 (the name-table entry patched above).  No jump
   // table repoint needed on Steam.
   constexpr uintptr_t lowres_prone_case_imm     = 0x006491CA;

   // ---- Entity / Vehicle (Carrier/Flyer) -------------------------------------

   constexpr uintptr_t carrier_vtable            = 0x0079A34C;  // EntityCarrier vftable (from ctor 0x00496f60)
   constexpr uintptr_t carrier_update            = 0x004971D0;  // EntityCarrier +0x240 vtable (0x79a1bc) slot 1
   constexpr uintptr_t carrier_kill              = 0x00497110;  // EntityCarrier +0x140 vtable (0x79a470) slot 1
   constexpr uintptr_t flyer_render              = 0x004AB040;  // EntityCarrier +0x94  vtable (0x79a49c) slot 19

   // ---- Debug / Visualization ------------------------------------------------

   constexpr uintptr_t freecam_update            = 0x0052D7B0;  // FreeCamera vtable (0x79f304) slot 1

   // ---- Vehicle view toggle (FP/TP) ------------------------------------------

   constexpr uintptr_t veh_view_hover_vtable_3c_slot     = 0x0079bbe0; // EntityHover +0x258 vtable (0x79bba4) + 0x3c
   constexpr uintptr_t veh_view_walker_vtable_3c_slot    = 0x0079dadc; // EntityWalker +0x258 vtable (0x79daa0) + 0x3c
   constexpr uintptr_t veh_view_cmd_hover_vtable_3c_slot  = 0x00798454; // CommandHover +0x258 vtable (0x798418) + 0x3c
   constexpr uintptr_t veh_view_cmd_walker_vtable_3c_slot = 0x0079881c; // CommandWalker +0x258 vtable (0x7987e0) + 0x3c
   constexpr uintptr_t veh_view_hover_3c_orig_thunk      = 0x00478db0; // const-true (MOV AL,1; RET)
   constexpr uintptr_t veh_view_walker_3c_orig_thunk     = 0x0047fc70; // const-true (MOV AL,1; RET)
   constexpr uintptr_t veh_view_return_false_thunk       = 0x004774c0; // const-false (XOR AL,AL; RET)

   // ---- Weapon / Grappling Hook ----------------------------------------------

   constexpr uintptr_t grapple_dtor              = 0x005ff360;  // ~OrdnanceGrapplingHook

   // ---- Animation (weapon/soldier) -------------------------------------------

   constexpr uintptr_t set_weapon_anim_map       = 0x0063f7b0;  // SetWeaponAnimationMap
   // FirstPerson::AddBank — the FP-anim-bank feature's AddBank (not
   // SoldierAnimationBank::AddBank 0x63c460, which is a different function the
   // old value pointed at).  Release (LTCG) passes the char* in ECX; modtools is
   // __cdecl — see the convention switch in soldier_fp_animation_override.cpp.
   constexpr uintptr_t anim_add_bank             = 0x00520EC0;

   // ---- Animation bank appending (anim_bank_append.cpp) -----------------------

   // AnimationFinder resolve loop (modtools FUN_0057f860). thiscall, 3 stack
   // args, RET 0xC — same shape as modtools; reads finder->mAnimBank (+0x20C).
   constexpr uintptr_t anim_finder_resolve       = 0x00644FE0;
   // SoldierAnimatorClass::FindAnimation inline slab loop (modtools FUN_0057de40).
   // thiscall(hash, name) RET 8 — still cleans 2 args but the name is unused:
   // Steam inlined RedAnimation::FindAnimation away and calls the ZephyrAnimBank
   // finder on bankEntry+0x14 directly.
   constexpr uintptr_t anim_class_find_in_banks  = 0x006442A0;
   // Global PblHashTableCode for RedAnimation (size 0x800, from _AddBank disasm).
   constexpr uintptr_t anim_hash_table           = 0x0099E80C;
   // ZephyrAnimBank find — thiscall(ECX=ZephyrAnimBank*, hash), RET 4.
   // NOTE: no red_find_animation on Steam (inlined; standalone body stripped) —
   // anim_bank_append branches on that field being 0.
   constexpr uintptr_t zephyr_anim_bank_find     = 0x0072BA40;

   // ---- Hashing (thiscall wrapper) -------------------------------------------

   constexpr uintptr_t hash_string_thiscall      = 0x00726d20;  // PblHash::PblHash

   // ---- Debug / Logging ------------------------------------------------------

   constexpr uintptr_t game_log                    = 0x006f6ff0; // RedWarning::LogMessage

   // Logging enablement (port of PrismaticFlower's upstream 3664782): retail
   // builds ship RedWarning file logging compiled but disabled.  After
   // RedWarning::Init (detoured), raise the file destination's min severity to
   // 0 and set pcLoggingEnabled — the same work the modtools build does itself
   // (modtools leaves these unmapped; feature no-ops there).
   constexpr uintptr_t red_warning_init            = 0x006F6EA0; // void()
   constexpr uintptr_t red_warning_set_dest_min_severity = 0x006F7280; // void(int dest, int minSeverity)
   constexpr uintptr_t pc_logging_enabled          = 0x01EAEDE6; // bool

   // ---- Shell / GC Visual Limits -----------------------------------------------
   // Release-build (LTCG) Add functions use custom register conventions (the
   // particle Add takes size in XMM3), so unlike modtools we do NOT detour them
   // — the limits are raised with pure byte patches instead:
   //   gc_*_count_patches      — disp32 operands of the count field (beam 0xB1C,
   //                             particle 0xE1C) across Add/Render/PostLoadHack
   //   gc_beam_limit_imm8_op   — CMP EAX,0x40 imm8 in beam Add (max value 0xFF,
   //                             so the Steam beam limit is 255, not 256)
   //   gc_particle_limit_imm32_op — CMP ECX,0x80 imm32 in particle Add
   //   gc_*_alloc_size_op      — PUSH imm32 operator new sizes in PostLoadHack
   constexpr uintptr_t gc_beam_add                  = 0x0057FF60;  // __cdecl-like, 7 stack args, RET 0x1C
   constexpr uintptr_t gc_particle_add              = 0x005802C0;  // 4 stack args + size in XMM3, RET 0x10

   constexpr uintptr_t gc_beam_count_patches[]      = {
       0x0057FF6C, 0x0057FF89,                          // Add (read/write)
       0x0057FBC0, 0x0057FF2A, 0x0057FF38, 0x0057FF49,  // DrawAllBeamBetween::Render
       0x0058059E,                                      // PostLoadHack count reset
   };

   constexpr uintptr_t gc_particle_count_patches[]  = {
       0x005802D2, 0x005802FC,                          // Add (read/write)
       0x005801C2, 0x00580299, 0x005802A7,              // DrawAllParticleAt::Render
       0x0058054A,                                      // PostLoadHack count reset
   };

   constexpr uintptr_t gc_beam_limit_imm8_op        = 0x0057FF72;
   constexpr uintptr_t gc_particle_limit_imm32_op   = 0x005802D8;
   constexpr uintptr_t gc_particle_alloc_size_op    = 0x00580515;
   constexpr uintptr_t gc_beam_alloc_size_op        = 0x00580563;

   // ---- RedParticleRenderer (see modtools namespace for docs) -------------------
   // Same cache layout as modtools: 15 caches x 0x3558 bytes, 200-entry cap.
   constexpr uintptr_t rpr_submit_particle          = 0x006D33A0;
   constexpr uintptr_t rpr_current_cache            = 0x009661B4;
   constexpr uintptr_t rpr_cache_index              = 0x009661B8;
   constexpr uintptr_t rpr_setcache_base_operand    = 0x006D3306;
   constexpr uintptr_t rpr_setcache_limit_imm8_op   = 0x006D32EA;

   // ---- Particle / Renderer Cache (BSS globals) --------------------------------

   constexpr uintptr_t s_cached_particles            = 0x01EF5120;
   constexpr uintptr_t s_caches                      = 0x009661E0;

   // ---- Controller / Input -------------------------------------------------------

   constexpr uintptr_t controller_base_global   = 0x01EBDC50;
   constexpr uintptr_t num_joysticks_global     = 0x0099CD08;
   constexpr uintptr_t joystick_config_base     = 0x01EF90D0;
   constexpr uintptr_t joystick_discover        = 0x0061D250;
   constexpr uintptr_t joystick_sync            = 0x0061D590;

   // ---- Rumble -------------------------------------------------------------------

   constexpr uintptr_t rumble_light_output      = 0x006C5550;
   constexpr uintptr_t rumble_heavy_output      = 0x006C5540;
   constexpr uintptr_t rumble_dispatch          = 0x00630D60;
   constexpr uintptr_t s_game_over              = 0x01E5600A;

   // ---- Aim Assist ---------------------------------------------------------------

   constexpr uintptr_t player_controller_update       = 0x0061A2B0;
   constexpr uintptr_t apply_damage                   = 0x00489340;
   constexpr uintptr_t lockon_mgr_array               = 0x01E57400;
   constexpr uintptr_t get_cur_wpn                    = 0x00484310;
   constexpr uintptr_t set_target_locked_obj          = 0x004844A0;
   constexpr uintptr_t m_camera_global                = 0x007f58e0;
   constexpr uintptr_t team_get_objects_in_range      = 0x006552d0; // LTCG convention: ECX=pos, EDX=out, XMM1=radius, stack=(max,team,flags,exclude) — NOT cdecl; use the aim_assist release thunk

   constexpr uintptr_t anim_finder_add_bank     = 0x00645a00;
   constexpr uintptr_t carrier_attach_cargo     = 0x00497300;
   constexpr uintptr_t carrier_take_off         = 0x004b3c60;
   // NOTE: 0x6569c0 (old value of cloth_enforce_collisions) is actually
   // TentacleSimulator::EnforceCollisions — the 0x656/0x657 "EntityCloth::"
   // labels in the Steam Ghidra DB are misported.  The real EntityCloth sim
   // lives at 0x458xxx-0x45bxxx (InternalUpdate 0x45adb0: frame gate +0x80 →
   // AccumulateForces 0x45af10 → Verlet 0x45ae10 → SatisfyConstraints →
   // ComputeNormals 0x45bd50).  Struct offsets match modtools: +0x20 pos,
   // +0x24 oldPos, +0x114 clothData ([0]=total,[1]=fixed), +0x110 class.
   constexpr uintptr_t cloth_enforce_collisions = 0x0045BA40;  // EntityCloth::EnforceCollisions — thiscall(matrix, hashtable), RET 8
   constexpr uintptr_t cloth_satisfy_constraints = 0x0045BC40; // EntityCloth::SatisfyConstraints — thiscall(matrix, hashtable, iterations), RET 0xC
   // LTCG: ECX=this, one stack arg (float* matrix, RET 4), halfHeight in
   // XMM2, radius in XMM3 — needs the naked thunk in cloth_collision_fix.cpp.
   constexpr uintptr_t cloth_enforce_cylinder_coll = 0x0045B7C0;
   constexpr uintptr_t load_config_real         = 0x005777e0;
   constexpr uintptr_t load_data_file_real      = 0x00577620;
   constexpr uintptr_t load_end_real            = 0x00576b90;
   constexpr uintptr_t mem_pool_alloc           = 0x006dc370;
   constexpr uintptr_t pbl_read_next_data       = 0x00727e30;
   constexpr uintptr_t pbl_read_next_scope      = 0x00727eb0;
   constexpr uintptr_t progress_set_all_on      = 0x00578c00;

   constexpr uintptr_t GameSound_play           = 0x00538010;
   constexpr uintptr_t red_pose_convert_skel32  = 0x006ddb30;
   constexpr uintptr_t snd_engine_update        = 0x00734590;
   constexpr uintptr_t zephyr_pose_dyn_set_anim = 0x0072d430;
   constexpr uintptr_t zephyr_pose_static_ctor  = 0x0072da90;
   constexpr uintptr_t zephyr_pose_static_open  = 0x0072df20;
   constexpr uintptr_t zephyr_skeleton_open     = 0x0072cc40;
   constexpr uintptr_t anim_add_skeleton_bank   = 0x00644570;
   constexpr uintptr_t carrier_initiate_landing = 0x004b3d50;
   constexpr uintptr_t draw_line_3d             = 0x006f12a0;
   constexpr uintptr_t draw_sphere              = 0x006f0a70;
   constexpr uintptr_t pbl_config_copy_ctor     = 0x00727de0;
   constexpr uintptr_t spline_build             = 0x0072a710;
   constexpr uintptr_t zephyr_pose_static_blend = 0x0072dc80;
   constexpr uintptr_t zephyr_pose_static_dtor  = 0x0072db10;
   constexpr uintptr_t zephyr_pose_static_set   = 0x0072dfd0;
   constexpr uintptr_t zephyr_skeleton_finalize = 0x0072bc30;

   constexpr uintptr_t anim_find_animation       = 0x00520f50;  // FindAnimation (name-matched)
   constexpr uintptr_t carrier_detach_cargo      = 0x00497410;  // DetachCargo (name-matched; adjacent to carrier_attach_cargo)
   constexpr uintptr_t console_add_variable      = 0x0041fe80;  // RedCommandConsole::AddVariable (name-matched)
   constexpr uintptr_t disguise_raise            = 0x00682f30;  // WhoTwiggedMe (name-matched)
   constexpr uintptr_t load_update_real          = 0x00576c00;  // LoadDisplay Update (adjacent to load_end_real)
   constexpr uintptr_t weapon_signal_fire        = 0x00679610;  // SignalFire (name-matched)
   constexpr uintptr_t snd_sound_play            = 0x0073a430;  // Snd::Play (gate: 7 shared callees + 2-source)
   constexpr uintptr_t passenger_activate        = 0x004dd7c0;  // ActivatePhysics (2-source agreement)
   constexpr uintptr_t rumble_state_setup        = 0x006b2790;  // rumble state setup (2-source agreement)

   // ---- Flyer path following / engine sound --------------------------------
   constexpr uintptr_t path_follower_land_jg     = 0x004D52FD;  // EntityPathFollower::Update — JG gating the LandOnArrival check to node 0 (7F 42)
   constexpr uintptr_t entity_flyer_land         = 0x004B3D50;  // EntityFlyer::Land (== carrier_initiate_landing)
   constexpr uintptr_t path_follower_reset       = 0x004D2970;  // EntityPathFollower::Reset — __thiscall, RET 4 (class arg unused on release)
   constexpr uintptr_t vehicle_engine_update     = 0x0066CCB0;  // VehicleEngine::Update — LTCG: ECX=this, XMM2=dt, 13 stack args, RET 0x34

   constexpr uintptr_t aimer_activate            = 0x0043e380;  // Aimer::ActivatePhysics (decompile-identical)
   constexpr uintptr_t get_weapon_anim_map       = 0x0063c970;  // SoldierAnimationBank::FindMap (decompile-identical table scan)
   constexpr uintptr_t snd_find_by_hash_id       = 0x00736a90;  // Properties::FindByHashID (decompile-identical list scan)
   constexpr uintptr_t carrier_update_landed_ht  = 0x004974b0;  // EntityCarrier::UpdateLandedHeight (unique name + bridge agree; adjacent to carrier_detach_cargo)
   constexpr uintptr_t disguise_drop             = 0x00683090;  // WeaponDisguise::FinishDroppingDisguise (diff + independent label; adjacent to disguise_raise)
   constexpr uintptr_t load_render_real          = 0x00576f10;  // LoadDisplay::Render (bridge + LoadDisplay CU cluster with load_update_real/load_end_real)
   constexpr uintptr_t render_screen_real        = 0x00577280;  // LoadDisplay::RenderScreen (CU cluster; same render call shape)
   constexpr uintptr_t platform_render_texture   = 0x00423980;  // PlatformRenderTexture (called from render_screen_real at matching site; arg shape matches)
   constexpr uintptr_t disguise_set_property     = 0x00683430;  // WeaponDisguiseClass::SetProperty (unique hit of key hash 0x8da6fec5 in .text)
   constexpr uintptr_t game_model_table          = 0x01ec1234;  // GameModel hash table (positional _Find call match in building SetProperty; xref count 12 vs 14)

   // ---- Weapon / Animated Lightsaber Textures (see modtools for docs) ----------
   // Steam/GOG _RenderLightsabre is an LTCG hybrid (ECX=pos, EDX=dir, 5 stack
   // args, caller-clean).  Blade array is at WeaponMeleeClass+0x2C8.
   constexpr uintptr_t lightsabre_render            = 0x0068F260;
   constexpr uintptr_t weaponmelee_set_property     = 0x0068D880;
   constexpr uintptr_t blade_lookup                 = 0x0068ED40;
   constexpr uintptr_t blade_global_model           = 0x01FAC3A0;
   constexpr uintptr_t blade_global_namehash        = 0x01FAC39C;
   constexpr uintptr_t blade_global_unk             = 0x01FAC3A4;

   // ---- HUD Widescreen Reticle Correction (see modtools for docs) --------------
   // Retail is SSE: replace the 24-byte MULSS...ADDSS region in
   // ReticuleDisplay::Update with a CALL to our correction thunk.
   constexpr uintptr_t reticle_display_update       = 0x00630650; // ReticuleDisplay::Update __thiscall bool(this,float)
   constexpr uintptr_t hud_screen_width             = 0x0093E4A4;
   constexpr uintptr_t hud_screen_height            = 0x0093E4A8;
   constexpr uintptr_t hud_reticle_mulss_patch      = 0x006308F6; // start of 24-byte MULSS..ADDSS region

   // ---- Lua core / character system (ported 2026-07-20) ------------------------
   // Character slot layout is build-INVARIANT (verified via Lua_Callbacks::
   // GetCharacterUnit 0x58fcd0 / GetCharacterTeam 0x58f820: stride 0x1B0,
   // ctrl +0x148, team +0x134).  Team struct likewise (+0x48 count, +0x50
   // classDefs, +0x54 min, +0x58 max — Team::FindUnitClassSlot 0x654020,
   // Team::_SetClassUnitCounts 0x6548c0).
   constexpr uintptr_t char_exit_vehicle   = 0x004F1380;  // EntitySoldier::ExitVehicle (vtable 0x79cf2c slot +0x68)
   constexpr uintptr_t char_array_base     = 0x01E30334;  // Character::sCharacters
   constexpr uintptr_t max_chars           = 0x01E30330;  // Character::sCharacterLimit
   constexpr uintptr_t team_array_base     = 0x007E9AA0;  // g_ppTeams (double-deref like modtools)
   constexpr uintptr_t class_def_list      = 0x007EC560;  // Factory<Entity,EntityClass,EntityDesc>::sList (node+0x4 next, +0xC def; def+0x18 hash)
   constexpr uintptr_t aimer_set_weapon    = 0x0043E400;  // Aimer::SetWeapon (EntitySoldier ctor fixup @0x4defa2)
   constexpr uintptr_t lua_create_entity   = 0x0058EB20;  // Lua_Callbacks::CreateEntity (reg table 0x7e7830)
   constexpr uintptr_t enter_state_path_op = 0x00577661;  // "Load\\load" MOV ECX,imm32 operand in LoadDataFile (release: MOV not PUSH)

   // netEnabled/netEnabledNext are laid out in REVERSE order vs modtools —
   // semantics verified against the Phantom-named CMOVNZ idiom in the
   // EntitySoldier ctor @0x4defa7: inShell ? netEnabledNext : netEnabled.
   constexpr uintptr_t net_in_shell        = 0x007E8007;
   constexpr uintptr_t net_enabled         = 0x01E62EA9;
   constexpr uintptr_t net_enabled_next    = 0x01E62EA8;

   // ---- First person (from FirstPerson::Init 0x521000) -------------------------
   // FirstPersonRenderable is 0x1650 bytes on release too; mCurrentWeapon
   // +0x1600 confirmed via the Phantom PDB struct (build-invariant).
   constexpr uintptr_t fp_renderable       = 0x01E55F00;  // FirstPerson::s_pRenderable[0]
   constexpr uintptr_t fp_anim_array       = 0x01E55E30;  // FirstPerson::mAnim[48]
   constexpr uintptr_t anim_name_table     = 0x00789760;  // FirstPersonAnimName[48]
   // FirstPersonRenderable::UpdateSoldier — found as FirstPerson::Update's
   // TYPE_SOLDIER (case 0) callee; writes +0x15fc/+0x160c/+0x1610/+0x15f8 and
   // reads +0x1608 (cached state) / +0x1600 (mCurrentWeapon), identical to the
   // modtools body (FP renderable layout is build-invariant).
   constexpr uintptr_t fp_update_soldier   = 0x0051FB70;

   // ---- Fog (see modtools namespace for docs) ----------------------------------
   constexpr uintptr_t red_renderer_set_fog_range  = 0x006B3640;
   constexpr uintptr_t red_renderer_set_fog_enable = 0x006B3620;
   constexpr uintptr_t fl_fog_start        = 0x008F6DBC;  // FLRenderer::m_fFogStart (RenderFarScene @0x6bd940 reads)
   constexpr uintptr_t fl_fog_end          = 0x008F6DC0;  // FLRenderer::m_fFogEnd

} // namespace steam

// =============================================================================
// BattlefrontII.exe GOG
// =============================================================================
namespace gog {

   // ---- Particle / Renderer Cache (BSS globals) --------------------------------

   constexpr uintptr_t s_cached_particles            = 0x01EF6640;
   constexpr uintptr_t s_caches                      = 0x00967680;

   // ---- Controller / Input -------------------------------------------------------

   constexpr uintptr_t controller_base_global   = 0x01EBF100;
   constexpr uintptr_t num_joysticks_global     = 0x0099E1A8;
   constexpr uintptr_t joystick_config_base     = 0x01EFA590;
   constexpr uintptr_t joystick_discover        = 0x0061E2B0;
   constexpr uintptr_t joystick_sync            = 0x0061E5F0;

   // ---- Rumble -------------------------------------------------------------------

   constexpr uintptr_t rumble_light_output      = 0x006C65E0;
   constexpr uintptr_t rumble_heavy_output      = 0x006C65D0;
   constexpr uintptr_t rumble_dispatch          = 0x00631E00;
   constexpr uintptr_t s_game_over              = 0x01E574B6;

   // ---- Aim Assist ---------------------------------------------------------------

   constexpr uintptr_t player_controller_update       = 0x0061B320;
   constexpr uintptr_t apply_damage                   = 0x00489340;
   constexpr uintptr_t lockon_mgr_array               = 0x01E588B0;
   constexpr uintptr_t get_cur_wpn                    = 0x00484310;
   constexpr uintptr_t set_target_locked_obj          = 0x004844A0;
   constexpr uintptr_t m_camera_global                = 0x007f6d80;
   constexpr uintptr_t team_get_objects_in_range      = 0x00656370; // same LTCG convention as steam (byte-identical fn) — use the aim_assist release thunk

   // ---- Flyer path following / engine sound (byte-identical to steam except
   //      vehicle_engine_update at the usual GOG +0x10A0 shift) ------------------
   constexpr uintptr_t path_follower_land_jg     = 0x004D52FD;
   constexpr uintptr_t entity_flyer_land         = 0x004B3D50;
   constexpr uintptr_t path_follower_reset       = 0x004D2970;
   constexpr uintptr_t vehicle_engine_update     = 0x0066DD50;

   // ---- Debug / Logging (see steam namespace for docs) -------------------------

   constexpr uintptr_t red_warning_init            = 0x006F7F70;
   constexpr uintptr_t red_warning_set_dest_min_severity = 0x006F8350;
   constexpr uintptr_t pc_logging_enabled          = 0x01EB029A;

   // ---- Hashing / Terrain (port of upstream 4a8d0df) ---------------------------

   constexpr uintptr_t pbl_hash_table_find       = 0x00727ed0;  // PblHashTableCode::_Find
   constexpr uintptr_t tex_hash_table            = 0x008f022c;  // RedTexture PblHashTable
   constexpr uintptr_t read_terrain              = 0x006c34f0;
   constexpr uintptr_t terrain_null_detail_texture = 0x009ca6cc;
   constexpr uintptr_t terrain_white_texture       = 0x009ca6d0;

   // ---- BlurEffect downsize clamp (see steam namespace for docs) ---------------

   constexpr uintptr_t blur_effect_render              = 0x0040f8d0;
   constexpr uintptr_t red_renderer_get_viewport_extents = 0x006b9730;

   // ---- Screenshot redirect (see steam namespace for docs) ---------------------

   constexpr uintptr_t d3d_device                      = 0x007f6dec;
   constexpr uintptr_t screenshot_request_call_site    = 0x00534290;

   // ---- RedWarning::DialogBoxMessage fix (see steam namespace for docs) --------

   constexpr uintptr_t red_warning_dialog_call_site    = 0x006f7c39;

   // ---- GameState (DLC mission-list init fix) -----------------------------------

   constexpr uintptr_t gamestate_shell_state           = 0x007ec968;
   constexpr uintptr_t gamestate_shell_state_enter     = 0x0053c6d0;
   constexpr uintptr_t gamestate_mission_state_enter   = 0x0053c830;

   // ---- Weapon / Animated Lightsaber Textures (see modtools for docs) ----------
   // Same LTCG hybrid convention as Steam.  Blade array at WeaponMeleeClass+0x2C8.
   constexpr uintptr_t lightsabre_render            = 0x006902F0;
   constexpr uintptr_t weaponmelee_set_property     = 0x0068E910;
   constexpr uintptr_t blade_lookup                 = 0x0068FDD0;
   constexpr uintptr_t blade_global_model           = 0x01FAD850;
   constexpr uintptr_t blade_global_namehash        = 0x01FAD84C;
   constexpr uintptr_t blade_global_unk             = 0x01FAD854;

   // ---- HUD Widescreen Reticle Correction (see modtools for docs) --------------
   // ReticuleDisplay::Update — derived from Steam (0x00630650) + the region's
   // Steam->GOG shift (0x10A0, from the mulss-patch pair); guarded at runtime by
   // a prologue check so a wrong guess disables the GOG refresh instead of crashing.
   constexpr uintptr_t reticle_display_update       = 0x006316F0;
   constexpr uintptr_t hud_screen_width             = 0x0093F944;
   constexpr uintptr_t hud_screen_height            = 0x0093F948;
   constexpr uintptr_t hud_reticle_mulss_patch      = 0x00631996;

} // namespace gog

} // namespace game_addrs
