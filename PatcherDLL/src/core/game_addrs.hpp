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
   constexpr uintptr_t lua_gettable      = 0x7B8770;  // calls luaV_gettable. NOTE: Ghidra mislabels 0x7B89A0 "lua_gettable" but that is lua_rawset (calls luaH_set + pops 2) — using it underflowed the Lua stack and spun the VM.
   constexpr uintptr_t lua_pcall         = 0x7B8B60;
   constexpr uintptr_t lua_rawgeti       = 0x7B8810;
   constexpr uintptr_t lua_settop        = 0x7B7E70;
   constexpr uintptr_t lua_insert        = 0x7B7F20;
   constexpr uintptr_t lua_newtable      = 0x7B8860;

   // Stock Lua callback: CreateEntity(class, matrix, name). Detoured to apply
   // VehicleSpawn-style post-create fixup (team + activate) so vehicles
   // spawned this way can actually fire weapons.
   constexpr uintptr_t lua_create_entity = 0x00472730;

   // ---- Aimer / Weapon -------------------------------------------------------

   // Aimer::SetSoldierInfo(Aimer*, PblVector3* pos, PblVector3* dir)
   constexpr uintptr_t aimer_set_soldier_info = 0x5EE9D0;

   // WeaponCannon vtable entry for OverrideAimer (vtable slot 0x70)
   constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0xA524D8;

   // Same slot on WeaponLauncher (vtable 0xA53AE8), which derives from WeaponCannon
   // but carries its own vtable.  It overrides seven slots, none of them Fire — it
   // inherits WeaponCannon::Fire (0x626490) — so the same hook applies unchanged.
   constexpr uintptr_t weapon_launcher_vftable_override_aimer = 0xA53B58;

   // Weapon::OverrideAimer implementation and thunk
   constexpr uintptr_t weapon_override_aimer_impl  = 0x61CEE0;
   constexpr uintptr_t weapon_override_aimer_thunk = 0x4068DE;

   // Weapon::ZoomFirstPerson() — returns true if weapon is in first-person zoom
   constexpr uintptr_t weapon_zoom_first_person = 0x61B640;

   // ScopeDisplay* — one-element array indexed by camera (PC only ever uses 0).
   // instance+0x4C9 is the bool ScopeDisplay::Update maintains for "scope texture
   // is on screen"; ScopeDisplay::Hide (0x683CB0) clears it.
   constexpr uintptr_t scope_display_instance = 0x00BA36D8;

   // float __cdecl CollisionManager::RayHit(PblVector3* start, PblVector3* dir,
   //     float maxDist, CollisionObject** outHit, PblVector3* outNormal,
   //     GameObject** exclude, int excludeCount, int flags, bool);
   // Returns the hit fraction of maxDist (1.0 = nothing hit).  Debug build: plain
   // cdecl, result in ST(0).  ILT thunk 0x407581.
   constexpr uintptr_t collision_manager_ray_hit = 0x0042E230;

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

   // Red3DModelElementLite::SetModel(const char* name) — __thiscall, RET 4.
   // Hashes the name and hands off to the by-hash variant (0x00839ea0), which
   // stores the hash at elem+0x74, resolves it against the global RedModel table
   // and writes the result to elem+0x78, setting the searchForModel bit at
   // elem+0x80 on a miss.  The loading screen calls this to bind team icon
   // models itself, because the engine's own TeamModel key is LoadDisplay-scope
   // only and so cannot vary per map.
   constexpr uintptr_t model_elem_set_model      = 0x00839f00;

   // LoadDataChunk and the four callees it dispatches to.  Detoured by
   // loading_screen/data_guard.cpp to bounds-check the fixed m_models[10] /
   // m_textures[50] / m_skeletons[10] arrays, which the stock loop appends to
   // with no checks at all.  See docs/LoadDisplaySystem.md.
   constexpr uintptr_t load_data_chunk_real      = 0x0067dea0;
   constexpr uintptr_t pbl_chunk_read_next_child = 0x007e4350;  // __thiscall(dst), RET 4
   constexpr uintptr_t red_model_read            = 0x007fa910;  // __cdecl(chunk) -> RedModel*
   constexpr uintptr_t red_texture_read          = 0x007fcec0;  // __cdecl(chunk) -> RedTexture*
   constexpr uintptr_t red_skeleton_read         = 0x00832d00;  // __cdecl(chunk) -> RedSkeleton*

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

   // ---- Map-queue next-mission fix (modtools-only defect) ---------------------
   // GameLoop::UpdateStats (0x00732ed0) is built from an older GameLoop.cpp than
   // Phantom/Steam: the "playlist had another map -> enter MissionState" branch
   // simply is not emitted, so every exit from the post-match stats screen lands
   // in the shell.  See map_queue_fix.cpp.
   //   ..._branch : `84 C0 75 2D` TEST AL,AL / JNZ shell, right after the
   //                CALL MissionPlayList::SelectNextEntry at 0x0073309c.
   //   ..._set_state : ILT thunk -> GameState::SetState (__cdecl, one hash arg;
   //                0x8ff60339 = MissionState, 0x11e1fc01 = ShellState).
   //   post_load_*  : NetPostLoad{Host,Join}Game** globals the shell path drains.
   constexpr uintptr_t updatestats_playlist_branch     = 0x007330a1;
   constexpr uintptr_t gamestate_set_state             = 0x00401af0;
   constexpr uintptr_t post_load_host_game             = 0x00c69268;
   constexpr uintptr_t post_load_join_game             = 0x00c6926c;

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
   // Despite the name this is GameSoundControllable::ReleaseVoice — thiscall on
   // the CONTROLLABLE (handle at +0, flags at +2), not Snd::Sound::VoiceVirtualRelease.
   constexpr uintptr_t voice_virtual_release       = 0x0074d440;
   // GameSoundControllable::Stop(bool hardStop) — __thiscall, RET 4.  The engine's
   // own teardown: ReleaseVoice then Snd::VoiceVirtual::Stop.  Use this instead of
   // poking Voice+0x80, which only imitates part of what VoiceVirtual::Stop does.
   constexpr uintptr_t gamesound_controllable_stop = 0x0074d470;
   // GameSoundControllable::StolenCallback — the completion callback Snd::Play
   // stores on the voice, so an owned one-shot is retired properly when the
   // engine steals its voice.  This is the ILT thunk; body at 0x0074d4c0.
   constexpr uintptr_t gamesound_stolen_callback   = 0x0040360c;
   // Snd::Sound::VoiceVirtualToVoiceVirtualHandle — __cdecl(VoiceVirtual*) ->
   // handle, i.e. (voice - smVoiceVirtuals) / 200 + 1.
   constexpr uintptr_t voice_to_handle             = 0x0088b5d0;
   constexpr uintptr_t snd_engine_update           = 0x008827b0;

   // Snd::SoundStream::Init() — clears the per-stream fire-and-forget state on
   // every engine open/close.  It unrolls its six smPlayingProps stores and six
   // smQueue flag ORs, so slots added by the Audio Stream Limit Increase patches
   // need the same treatment applied from outside (see audio_stream_limit.cpp).
   constexpr uintptr_t snd_soundstream_init        = 0x0088a450;

   // The `cmp reg, 6` immediate in Snd::EngineBase::GetFreeStream.  Read at
   // runtime to tell whether the Audio Stream Limit Increase patch set applied
   // (it may be off in the INI, or have failed verification).
   constexpr uintptr_t snd_stream_slot_count_imm8  = 0x008828cd;

   // Snd::EngineBase::GetFreeStream() — the ONLY allocator of a stream slot, and
   // reached from exactly one caller (the Lua OpenAudioStream callback).
   constexpr uintptr_t snd_engine_get_free_stream  = 0x008828b0;

   // ---- Snd::Properties field offsets (NOT addresses) --------------------------
   // Debug and release disagree by -4 from Properties+0x18 onward; see the steam
   // namespace for the release values and loading_screen/shared.hpp for use.
   constexpr uintptr_t snd_props_loop_byte       = 0x1c;  // bit 0x10 = looping
   constexpr uintptr_t snd_props_next_allowed    = 0x68;  // float, replay cooldown

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
   // RedWarning::SetLogData(severity, file, line, date, time) — __cdecl.  Sets the
   // context the bf2log formatter prints as "Message Severity: N ... file(line)".
   // Severity 3 = RED_SEVERITY_ERROR, what the engine uses for real errors.
   constexpr uintptr_t red_warning_set_log_data    = 0x007E37A0;
   // RedWarning::g_bFormatted (byte). When zero, LogMessage skips the
   // "Message Severity: N / file(line)" header and prints the raw text.
   constexpr uintptr_t log_formatted_flag          = 0x00CF6910;
   // DownloadableContent::GetContentDirectory() — __cdecl(void) -> char*, NULL
   // when no addon is active.  Returns the addon directory, e.g. "addon\\VTR".
   constexpr uintptr_t dlc_get_content_directory   = 0x00449400;

   // ---- Shell / In-game movies -------------------------------------------------
   // Lua_Callbacks::ScriptCB_PlayInGameMovie(L) — __cdecl lua_CFunction, registered
   // at 0x00AC7830.  Retail rewrote the dev-build version: it reads only Lua arg 2
   // (the segment name) and picks the movie FILE from a hardcoded language table
   // (ingame.mvs / ingamefr.mvs / ingamegr.mvs), so arg 1 is ignored entirely.
   constexpr uintptr_t scriptcb_play_ingame_movie  = 0x004653E0;
   // GameMovie::sInGameMovieFilename — 256-byte buffer StartInGameMoviePlay fills
   // with "Movies\\<file>" and UpdatInGameMovie hands to GameMovie::Open.
   constexpr uintptr_t ingame_movie_filename       = 0x00B30290;
   // GameMovie::sInGameMoviePlayerState — 0 idle, 1 requested, 2 opening, 3 playing.
   constexpr uintptr_t ingame_movie_player_state   = 0x00B30280;

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

   // ---- AILowLevel::UpdateIndirect null-target crash ------------------------
   // The squad-order branch nulls its target pointer for two states then virtual
   // -calls it regardless.  Guard site is the 6 bytes at 0x005A2B84
   // (MOV ECX,[ESP+8] ; MOV EDX,[EAX]); _resume continues into the call, _skip
   // is the engine's own "call returned false" continuation.
   constexpr uintptr_t ai_squad_order_guard_site   = 0x005A2B84;
   constexpr uintptr_t ai_squad_order_guard_resume = 0x005A2B8A;
   constexpr uintptr_t ai_squad_order_guard_skip   = 0x005A2BA9;

   // SoldierAnimator::ApplyProceduralAnimationAndBuildWorldMatrices — the sole
   // producer of the soldier's per-joint world matrices.  thiscall(this, float),
   // RET 4.  Tail is PUSH gMatrixIdentity / LEA ECX,[EBX+0xd0] / CALL
   // ZephyrSkeleton<32>::BuildWorldMatrices (0x0082c390, which Ghidra currently
   // mislabels "Finalize" — it writes m_pWorldMatrices[0] then recurses down the
   // m_iChild chain).  Used by entity/soldier_ragdoll.cpp.
   constexpr uintptr_t soldier_apply_procedural    = 0x00579F10;

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

   // The PblHash value WeaponShield::Update pushes into EntityDroideka::IsA at
   // its RTTI check (@0x63f3e8: MOV ECX,[0xb7d934]; PUSH ECX; CALL [vtable+0]).
   // Read as a dword; entity/droideka_death_anim_fix.cpp reuses it to tell
   // droideka shield owners apart from soldier ones.
   constexpr uintptr_t entity_droideka_rtti_hash = 0x00B7D934;

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

   // ---- Combat awards (award_disable.cpp) --------------------------------------
   // bool __thiscall MedalsMgr::IsAwardAvailable(MedalsMgr*, int index) — the one
   // gate every award effect reads.  Body is
   //     gEnableAllAwards || (this->mAwardAvailable & (1 << index))
   // with mAwardAvailable a ushort at MedalsMgr+0x4C (same offset on all builds).
   // Award indices come from MedalsMgr::sMedalPoints[9]:
   //     0 gunslinger  1 frenzy   2 demolition  3 technician  4 marksman
   //     5 regulator   6 endurance  7 guardian  8 warhero
   // Never inlined — 23 call sites on all three builds.  ILT thunk 0x00403080.
   constexpr uintptr_t medals_is_award_available    = 0x00653E00;

   // bool __thiscall MedalsMgr::IsAwardAvailableInternal(MedalsMgr*, int index)
   // — "has this award been earned", from career level + medal points.  It is
   // what MedalsMgr::Update diffs IsAwardAvailable against to drive the bit and
   // the award-unlocked HUD message, so suppressing awards HERE keeps the two
   // sides agreeing and the HUD quiet.  Returns false outright on a net client
   // (that build gets its mask from MedalsMgr::NetRead instead).
   // Also read by MedalsMgr::ClearAllMedalPoints.  ILT thunk 0x004058F8.
   constexpr uintptr_t medals_is_award_available_internal = 0x00653D70;

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
   constexpr uintptr_t lua_newtable      = 0x69bdb0;

   // ---- Aimer / Weapon -------------------------------------------------------

   constexpr uintptr_t aimer_set_soldier_info = 0x0043d290;
   constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0x007b05ec; // WeaponCannon vftable (0x7b057c) + slot 28*4
   constexpr uintptr_t weapon_launcher_vftable_override_aimer = 0x007b1314; // WeaponLauncher vftable (0x7b12a4) + 0x70
   constexpr uintptr_t weapon_override_aimer_impl  = 0x00677780;          // Weapon::OverrideAimer (default `return 0`)
   constexpr uintptr_t weapon_override_aimer_thunk = 0x00677780;          // No ILT thunk in release build; same as impl
   constexpr uintptr_t weapon_zoom_first_person = 0x00677d40;

   // ScopeDisplay* — same +0x4C9 visible flag as modtools (the instance is 0x500
   // here vs 0x520 in the debug build, but only the trailing GameSound members
   // differ; ScopeDisplay::Hide 0x633B30 reads the same offset).
   constexpr uintptr_t scope_display_instance = 0x01EAF020;

   // CollisionManager::RayHit — same 9 arguments as modtools, but LTCG-custom:
   //   ECX = start, EDX = dir, XMM2 = maxDist, and the remaining six on the stack
   //   (outHit, outNormal, exclude, excludeCount, flags, bool), caller-cleans,
   //   result in XMM0.  Call it through rayhit_release_thunk(), never directly.
   constexpr uintptr_t collision_manager_ray_hit = 0x0045E3A0;
   constexpr uintptr_t weapon_update            = 0x006781B0;             // Weapon vtable (0x7b01a8) slot 1
   constexpr uintptr_t weapon_shield_update     = 0x00691A80;             // WeaponShield vtable (0x7b1a9c) slot 1

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

   // Third unguarded Controllable::mPilot deref, this one release-only (the
   // byte pair does not occur anywhere in the modtools .text).  Reached from
   // EntityCarrier cargo activation: cargo vtable[5] (0x500410) walks its
   // children and virtual-calls each via `CALL [EAX+0x7c]` @0x500592; the
   // callee 0x5005c0 then does
   //     00500634  8B 86 D0 00 00 00   MOV EAX,[ESI+0xd0]     ; mPilot
   //     0050063a  FF B0 D4 00 00 00   PUSH [EAX+0xd4]        ; AV when null
   // on the branch taken when [ESI+0xd4] >= 0, with no null check — while the
   // sibling branch at 0x500647 checks mPilot properly and jumps to 0x5006dd.
   // Carried cargo has no pilot, so the guard mirrors the engine's own handling
   // and jumps to that same convergence point.
   constexpr uintptr_t activate_pilot_crash_site  = 0x00500634;
   constexpr uintptr_t activate_pilot_skip_target = 0x005006dd;

   // ---- AILowLevel::UpdateIndirect null-target crash -------------------------
   // AILowLevel::UpdateIndirect = FUN_0043bc80 (modtools 0x005A2A80).  Retail
   // codegen differs from modtools: the (possibly null) target is in ECX and the
   // guard site is the 7 bytes at 0x0043BD31
   // (MOV EAX,[ECX] ; PUSH 1 ; PUSH [EBP-4]).  Both pushes sit after the
   // dereference, so the skip path has nothing pending and is a bare jump to the
   // engine's own "call returned false" continuation.
   constexpr uintptr_t ai_squad_order_guard_site   = 0x0043BD31;
   constexpr uintptr_t ai_squad_order_guard_resume = 0x0043BD38;
   constexpr uintptr_t ai_squad_order_guard_skip   = 0x0043BD4A;

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

   // Same EntityDroideka RTTI hash value, read at WeaponShield::Update 0x691af9
   // (PUSH dword ptr [0x1ebbc58]).
   constexpr uintptr_t entity_droideka_rtti_hash = 0x01EBBC58;

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
   // ---- Flyer boost animation ----------------------------------------------
   // EntityFlyerClass::InitAnimations — writes mAnimObj [ESI+0x7b0], takeoffAnim
   // [ESI+0x7b4] and the alternate [ESI+0x7b8] from two AnimBankFind calls, same
   // PUSH 0x800 bank size / RET 4 / XOR AL,AL early-out as modtools 0x004F6560.
   constexpr uintptr_t flyer_init_animations     = 0x004b9720;
   // ---- Carrier turret fire ------------------------------------------------
   // MountedTurret::Update 0x005a5f50 (Ghidra had it as OrdnanceTowCable::
   // Update; it matches modtools 0x00565630 instruction for instruction —
   // same [this-0xC] parent, vtable+0x24 call, 2-arg cdecl helper, TEST AL,AL).
   // The fire gate is encoded differently here than on modtools, so the patch
   // length differs (18 vs 17) and the site had to be found by byte scan:
   //   005a64df 8B06           MOV EAX,[ESI]
   //   005a64e1 8BCE           MOV ECX,ESI
   //   005a64e3 8B4024         MOV EAX,[EAX+0x24]
   //   005a64e6 FFD0           CALL EAX
   //   005a64e8 83B864050000 00 CMP [EAX+0x564],0    (modtools: MOV/TEST 0x5A4)
   //   005a64ef 7529           JNZ +0x29
   constexpr uintptr_t turret_fire_check         = 0x005a64df;
   constexpr uintptr_t turret_fire_allow         = 0x005a64f1;  // check + 18
   constexpr uintptr_t turret_fire_block         = 0x005a651a;  // allow + 0x29
   // MountedTurret::UpdateIndirect — found by vtable slot alignment: it is
   // slot 49 of the MountedTurret sub-object vtable (modtools base 0x00a43360,
   // Steam base 0x007aa650), and slot 1 is Update on both (modtools thunk
   // 0x41370a -> 0x565630, Steam 0x5a5f50).  Body confirms it: [EDI+0xc8] AI,
   // LEA ESI,[EAX+0x2c4], same [ESI+0x50/54/5c/64] reads, and it writes the
   // heading controls to [EDI+0x88]/[EDI+0x8c] exactly like modtools.
   // dt is on the stack (MOVSS XMM1,[EBP+8]) and it ends RET 4, so the
   // bool __fastcall(ecx, edx, float) shape is correct here.
   constexpr uintptr_t turret_update_indirect    = 0x005a82d0;
   // The weapon fire state machine. CONVENTION DIFFERS: modtools 0x00562dd0 is
   // __thiscall(trigger, float dt, char fire) / RET 8, but this build has NO dt
   // parameter at all — __thiscall(trigger, char fire), fire at [EBP+8], RET 4.
   // The bitfield body is otherwise byte-identical (AND 0x1f / CMP 0x1c /
   // AND 0xffffffe1 / OR 1), which is how it was matched.
   constexpr uintptr_t trigger_update            = 0x0043a950;
   // ZephyrPoseDyn<32>::SetAnimTime and the global identity matrix, both read
   // out of EntityFlyer::Render 0x4AB040 at the points where modtools' Render
   // 0x4f6970 calls 0x0082A9C0 (004f6b2c) and pushes 0x00CF6830 (004f6b75).
   constexpr uintptr_t zephyr_pose_dyn_set_time  = 0x0072d650;
   constexpr uintptr_t g_identity_matrix         = 0x009caee0;

   // ---- Hashing (thiscall wrapper) -------------------------------------------

   constexpr uintptr_t hash_string_thiscall      = 0x00726d20;  // PblHash::PblHash

   // ---- Debug / Logging ------------------------------------------------------

   constexpr uintptr_t game_log                    = 0x006f6ff0; // RedWarning::LogMessage
   constexpr uintptr_t red_warning_set_log_data    = 0x006F71A0; // 5-arg formatted overload
   constexpr uintptr_t log_formatted_flag          = 0x009C8490;
   constexpr uintptr_t dlc_get_content_directory   = 0x0048EBA0; // returns addmeDirectory

   // ---- Shell / In-game movies -------------------------------------------------
   // See the modtools namespace for what these are.  Reg table 0x007E7058;
   // StartInGameMoviePlay (LTCG, ECX=file EDX=segment) is 0x00534BD0 and is where
   // the two globals below were read off.
   constexpr uintptr_t scriptcb_play_ingame_movie  = 0x00585790;
   constexpr uintptr_t ingame_movie_filename       = 0x01E56288;
   constexpr uintptr_t ingame_movie_player_state   = 0x01E5616C;

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
   // EntityCarrierClass::SetProperty — Ghidra-named; EntityCarrierClass vtable
   // (0x79a2b8, from ctor 0x497610) slot 6.  Same __thiscall(this, uint hash,
   // const char* value) shape as modtools (RET 0x8), and the cargo-node property
   // hashes are build-invariant (0x3e2c4da4 / 0x910a89fc appear as literal CMPs).
   constexpr uintptr_t carrier_set_property     = 0x004976b0;
   constexpr uintptr_t carrier_attach_cargo     = 0x00497300;
   // VehicleSpawn::UpdateSpawn — instruction-level match with modtools 0x665A50:
   // same `[this+0xF8]` team index, `[this+EAX*4+0x90]` spawn class, vtable+0x20
   // call and `LEA [this+0x110]`.  VehicleSpawn's own offsets are unchanged
   // between builds (it is not in the -0x40 shifted EntityFlyer/Carrier range).
   // VehicleSpawn::UpdateSpawn — CONVENTION DIFFERS FROM MODTOOLS.  Under LTCG
   // this build passes dt in XMM1 with no stack argument and ends in a bare
   // `RET` (0066fcf7), where modtools takes dt at [EBP+8] and ends `RET 4`.
   // Hooking it with the modtools __fastcall(ecx, edx, float) shape pops 4
   // bytes that were never pushed, corrupting ESP on every call — see the
   // regcall thunks in flyer_carrier_fixes.cpp.
   constexpr uintptr_t carrier_update_spawn     = 0x0066F370;
   // VehicleTracker::sMemoryPool — the `PUSH 0x1c; MOV ECX,<pool>; CALL
   // mem_pool_alloc` tracker allocation at the tail of UpdateSpawn (0066fc6a),
   // positionally identical to modtools 0x00665a50 -> pool 0x00B9A758.
   constexpr uintptr_t vehicle_tracker_pool     = 0x01f9a278;
   // GameLoop::sPauseMode — bool, true while ESC-paused.  GameLoop::Pause
   // 0x5334d0 / Resume 0x5334e0 are the same one-line `MOV byte [x],1` / `,0`
   // setter pair as modtools 0x733340 / 0x733350 -> 0x00c6aae8.
   constexpr uintptr_t gameloop_pause_mode      = 0x01e5605e;
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
   // LoadDataChunk + callees.  Derived 2026-07-27 from the Steam decompile of
   // LoadDisplay::LoadDataChunk; the struct offsets it uses (0x14dc/0x1504/
   // 0x15cc/0x15f4/0x15f8/0x15fc) are byte-identical to modtools.
   constexpr uintptr_t load_data_chunk_real      = 0x005776e0;
   constexpr uintptr_t pbl_chunk_read_next_child = 0x0072aa30;
   constexpr uintptr_t red_model_read            = 0x006c43f0;
   // Red3DModelElementLite::SetModel — already named in the Steam DB, and found
   // the same way it is used: LoadConfig's TeamModel branch (0x005777e0, hash
   // 0xd6c2b5f9) does PblHash -> GetTeamNum (0x00575550) -> `LEA this + slot*0x150
   // + 0x430` -> SetModel, instruction-for-instruction the modtools sequence.
   // Its by-hash tail is 0x006d6fc0, RedModel table 0x0093ebdc size 0x800.
   constexpr uintptr_t model_elem_set_model      = 0x006d7010;
   constexpr uintptr_t red_texture_read          = 0x006ba2d0;
   constexpr uintptr_t red_skeleton_read         = 0x006e88c0;
   constexpr uintptr_t mem_pool_alloc           = 0x006dc370;
   constexpr uintptr_t pbl_read_next_data       = 0x00727e30;
   constexpr uintptr_t pbl_read_next_scope      = 0x00727eb0;
   constexpr uintptr_t progress_set_all_on      = 0x00578c00;

   // ---- Loading screen, completed 2026-07-27 -----------------------------------
   // The seven symbols the loading_screen module still needed on retail, plus the
   // Play completion callback it used to pass as a raw modtools VA.
   //
   // PblConfig::PblConfig(PblFileChunk*) — sits directly ahead of the copy ctor,
   // same body as modtools 0x821000 minus the release-stripped 'NAME' assert.
   constexpr uintptr_t pbl_config_ctor          = 0x00727da0;
   // LoadDisplay::Update's QueryPerformanceCounter stamp (LowPart; HighPart at
   // +4).  Read to tell whether the engine repainted since our last injected
   // frame.  From LoadDisplay::Update 0x00576c00.
   constexpr uintptr_t load_update_qpc_stamp    = 0x01faaa70;
   // s_loadHeap — the heap index LoadDisplay::Update switches to.  Same site.
   constexpr uintptr_t s_loadheap_global        = 0x01f9c2e4;
   // GameMemory::RunTimeHeap — the "Runtime" heap created by GameMemory::
   // BuildHeaps 0x00533bc0 and made current there.
   constexpr uintptr_t runtime_heap_global      = 0x01e56160;
   // NOTE: color_ptr_global has no retail counterpart.  PlatformRenderTexture
   // args 6 (RedColor*) and 7 (alphaBlend) survive in the signature — the
   // release build still RETs 0x34 for 13 stack dwords — but the body never
   // reads either slot, and LoadDisplay::RenderScreen 0x00577280 does not even
   // bother storing them.  Left undefined (0) on purpose.
   //
   // Snd::Sound::VoiceVirtualToVoiceVirtualHandle — __cdecl, (voice -
   // smVoiceVirtuals 0x01e2b4c0) / 200 + 1, the exact inverse of 0x0073af60.
   constexpr uintptr_t voice_to_handle          = 0x0073afb0;
   constexpr uintptr_t voice_virtual_release    = 0x00538630;
   // GameSoundControllable::Stop(bool hardStop) — __thiscall, RET 4; identical
   // body to modtools 0x0074d470.
   constexpr uintptr_t gamesound_controllable_stop = 0x00538660;
   // GameSoundControllable::StolenCallback — __cdecl(voice, controllable).  No
   // ILT thunk on retail, so this is the body itself.
   constexpr uintptr_t gamesound_stolen_callback   = 0x00538730;

   // ---- Snd::Properties field offsets (NOT addresses) --------------------------
   // Release drops 4 bytes somewhere before Properties+0x18, so every field from
   // there on sits 4 lower than on modtools.  Both derived from the same two
   // sites: Snd::Sound::Play's `*(byte*)(props + loop) >> 4 & 1` and the replay
   // gate it calls, whose float[3] is nextAllowedTime.
   constexpr uintptr_t snd_props_loop_byte      = 0x18;
   constexpr uintptr_t snd_props_next_allowed   = 0x64;

   constexpr uintptr_t GameSound_play           = 0x00538010;
   constexpr uintptr_t red_pose_convert_skel32  = 0x006ddb30;
   constexpr uintptr_t snd_engine_update        = 0x00734590;
   constexpr uintptr_t snd_soundstream_init     = 0x00736b40;
   constexpr uintptr_t snd_stream_slot_count_imm8 = 0x0073409c;
   constexpr uintptr_t snd_engine_get_free_stream = 0x00734080;
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
   // WAS 0x004dd7c0, which is EntityRemoteTerminal::ActivatePhysics — the old
   // "2-source agreement" matched on the bare name ActivatePhysics and picked
   // the wrong class.  0x0060fab0 is byte-identical to modtools 0x00568540
   // (MOV EAX,[ECX+0xc] / ADD ECX,0xc / PUSH 2 / PUSH -0x14 / CALL [EAX+8]) and
   // is the function EntityFlyer::ActivatePhysics 0x4b5f70 actually calls for
   // each passenger slot.
   constexpr uintptr_t passenger_activate        = 0x0060fab0;  // PassengerSlot::ActivatePhysics
   // MountedTurret::ActivatePhysics — called per turret by EntityFlyer::
   // ActivatePhysics 0x4b5f70 at 004b5fe6 (modtools 0x00563a90).
   constexpr uintptr_t turret_activate           = 0x005a7b70;
   constexpr uintptr_t rumble_state_setup        = 0x006b2790;  // rumble state setup (2-source agreement)

   // ---- Flyer path following / engine sound --------------------------------
   constexpr uintptr_t path_follower_land_jg     = 0x004D52FD;  // EntityPathFollower::Update — JG gating the LandOnArrival check to node 0 (7F 42)
   constexpr uintptr_t entity_flyer_land         = 0x004B3D50;  // EntityFlyer::Land (== carrier_initiate_landing)
   constexpr uintptr_t path_follower_reset       = 0x004D2970;  // EntityPathFollower::Reset — __thiscall, RET 4 (class arg unused on release)
   constexpr uintptr_t vehicle_engine_update     = 0x0066CCB0;  // VehicleEngine::Update — LTCG: ECX=this, XMM2=dt, 13 stack args, RET 0x34

   constexpr uintptr_t aimer_activate            = 0x0043e380;  // Aimer::ActivatePhysics (decompile-identical)
   constexpr uintptr_t get_weapon_anim_map       = 0x0063c970;  // SoldierAnimationBank::FindMap (decompile-identical table scan)
   // Snd::Sound::Properties::FindByHashID — __cdecl(hash).
   //
   // FindByHashID is a template: the Phantom PDB has 32 of them, one per Snd
   // config class, and they decompile identically. 0x00736a90 was picked by
   // shape and is the WRONG sibling: it walks the list at 0x007e3584, whose
   // nodes are built by 0x007366f0 (reached from the 0x2e93ef4c/0x4ca38b31
   // chunk reader). Sound properties never appear in it, so every lookup
   // returned null while the lvl itself loaded perfectly — the loading screen's
   // "sound hash %08x not found".
   //
   // The right one walks 0x007e36f8, which is where the SoundProperties ctor
   // 0x00739b70 links each object (node at obj+0x84), and which Sound::Play
   // 0x0073a430 consumes: it reads props+0x58 and props+0x80, exactly the fields
   // that ctor initialises. Field shapes agree too — hash at node-0x80 = obj+4,
   // object base at node-0x84.
   constexpr uintptr_t snd_find_by_hash_id       = 0x00739d90;
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

   // ---- Combat awards (see modtools namespace for docs) ------------------------
   // Retail dropped the gEnableAllAwards debug flag, so the body is just the
   // mask test; 23 call sites, matching modtools/Phantom one for one.
   constexpr uintptr_t medals_is_award_available = 0x005A33D0;
   constexpr uintptr_t medals_is_award_available_internal = 0x005A3350;

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

   // =========================================================================
   // Ported from the Steam namespace with tools/port_gog.py (2026-07-26).
   //
   // The two retail exes are the same source built with the same toolchain, so
   // GOG .text is Steam .text shifted by a piecewise-constant amount (0 in the
   // low ranges, ~+0x1000 above 0x63xxxx) and the globals move with .data.
   // Every address below was predicted from that shift map and then verified by
   // comparing the two builds instruction by instruction: mnemonics, registers
   // and struct displacements must match exactly, only relocated VAs may differ.
   //
   // The same pass re-derived all 48 hand-mapped addresses that already existed
   // in this namespace and reproduced 47 of them exactly, with no contradiction
   // (pc_logging_enabled has no literal code reference to ride on, so the tool
   // abstains rather than disagreeing).
   //
   // Calling conventions are inherited from Steam unchanged -- GOG is the same
   // release/LTCG build, so every naked thunk written for Steam applies here.
   // =========================================================================

   // ---- Lua VM ------------------------------------------------------------------

   constexpr uintptr_t init_state                     = 0x005a1c90;
   constexpr uintptr_t g_lua_state_ptr                = 0x01e58e50;
   constexpr uintptr_t lua_pushcclosure               = 0x0069cfc0;
   constexpr uintptr_t lua_pushlstring                = 0x0069d090;
   constexpr uintptr_t lua_settable                   = 0x0069d460;
   constexpr uintptr_t lua_tolstring                  = 0x0069d640;
   constexpr uintptr_t lua_pushnumber                 = 0x0069d0f0;
   constexpr uintptr_t lua_tonumber                   = 0x0069d5a0;
   constexpr uintptr_t lua_gettop                     = 0x0069cc30;
   constexpr uintptr_t lua_pushnil                    = 0x0069d0d0;
   constexpr uintptr_t lua_pushboolean                = 0x0069cfa0;
   constexpr uintptr_t lua_toboolean                  = 0x0069d560;
   constexpr uintptr_t lua_touserdata                 = 0x0069d6d0;
   constexpr uintptr_t lua_pushlightuserdata          = 0x0069d070;
   constexpr uintptr_t lua_isnumber                   = 0x0069cd10;
   constexpr uintptr_t lua_gettable                   = 0x0069cbf0;
   constexpr uintptr_t lua_pcall                      = 0x0069cf40;
   constexpr uintptr_t lua_rawgeti                    = 0x0069d230;
   constexpr uintptr_t lua_settop                     = 0x0069d490;
   constexpr uintptr_t lua_insert                     = 0x0069cc90;
   constexpr uintptr_t lua_newtable                   = 0x0069ce40;

   // ---- Aimer / Weapon ----------------------------------------------------------

   constexpr uintptr_t aimer_set_soldier_info         = 0x0043d280;
   constexpr uintptr_t weapon_cannon_vftable_override_aimer = 0x007b1564;
   constexpr uintptr_t weapon_launcher_vftable_override_aimer = 0x007b228c;
   constexpr uintptr_t weapon_override_aimer_impl     = 0x00678820;
   constexpr uintptr_t weapon_override_aimer_thunk    = 0x00678820;
   constexpr uintptr_t weapon_zoom_first_person       = 0x00678de0;
   constexpr uintptr_t scope_display_instance         = 0x01eb04d4;
   constexpr uintptr_t collision_manager_ray_hit      = 0x0045e3a0;
   constexpr uintptr_t weapon_update                  = 0x00679250;
   constexpr uintptr_t weapon_shield_update           = 0x00692b10;

   // ---- Hashing / Texture lookup ------------------------------------------------

   constexpr uintptr_t hash_string                    = 0x00727f20;

   // ---- EntityHover self-piloted crash fix (see modtools notes) -----------------

   constexpr uintptr_t hover_updateindirect_pilot_call = 0x004c7036;
   constexpr uintptr_t controllable_get_active_pilot  = 0x0043aac0;
   constexpr uintptr_t hover_command_crash_site       = 0x004ece30;
   constexpr uintptr_t hover_command_skip_target      = 0x004ecb1e;
   // Same VAs as Steam and byte-identical (see the steam namespace for the
   // full note); this whole region is in the shift-0 range.
   constexpr uintptr_t activate_pilot_crash_site      = 0x00500634;
   constexpr uintptr_t activate_pilot_skip_target     = 0x005006dd;

   // ---- AILowLevel::UpdateIndirect null-target crash -----------------------------
   // Same codegen as Steam, shifted -0x10 (byte-identical at the site and at the
   // skip target); see the steam namespace for the full note.
   constexpr uintptr_t ai_squad_order_guard_site      = 0x0043BD21;
   constexpr uintptr_t ai_squad_order_guard_resume    = 0x0043BD28;
   constexpr uintptr_t ai_squad_order_guard_skip      = 0x0043BD3A;

   // ---- Memory heap management --------------------------------------------------

   constexpr uintptr_t red_set_current_heap           = 0x006c4ca0;

   // ---- Entity / Soldier Prone --------------------------------------------------

   constexpr uintptr_t EntitySoldier_prone            = 0x0079df6c;
   constexpr uintptr_t EntitySoldier_crouch           = 0x004ed550;
   constexpr uintptr_t EntitySoldier_stand            = 0x004ed080;
   constexpr uintptr_t EntitySoldier_SetState         = 0x004ee2c0;
   constexpr uintptr_t FoleyFXCollider_GetFoleyFX     = 0x004e7bf0;
   constexpr uintptr_t prone_anim_accessor            = 0x0063d370;
   constexpr uintptr_t SoldierAnimator_SetAction      = 0x0063fe00;
   constexpr uintptr_t prone_guard_jnz                = 0x004e8968;
   constexpr uintptr_t prone_acklay_gate_jnz          = 0x004e67c0;
   constexpr uintptr_t prone_height_jump_table        = 0x004f07bc;
   constexpr uintptr_t prone_height_switch_end        = 0x004f04f3;
   constexpr uintptr_t prone_primary_stance_and       = 0x00544334;
   constexpr uintptr_t WeaponMeleeClass_vftable       = 0x007b24ac;
   constexpr uintptr_t lua_read_data_file             = 0x0058bc00;
   constexpr uintptr_t load_util_read_data_file       = 0x0057a9a0;
   constexpr uintptr_t lowres_postload                = 0x00648de0;
   constexpr uintptr_t pbl_hash_table_store           = 0x00728030;
   constexpr uintptr_t pbl_temp_hash                  = 0x00727e50;

   // ---- Entity / Droideka DisableBallMode ---------------------------------------

   constexpr uintptr_t droideka_class_set_property    = 0x004a82a0;
   constexpr uintptr_t droideka_update_pilot          = 0x004a2030;
   constexpr uintptr_t droideka_class_derive          = 0x004a8180;

   // ---- Entity / Soldier override textures (OverrideTexture3..5) ----------------

   constexpr uintptr_t soldier_class_set_property     = 0x004f82e0;
   constexpr uintptr_t soldier_render                 = 0x004e23d0;
   constexpr uintptr_t soldier_element_render_ctx     = 0x0048dc90;
   constexpr uintptr_t shading_pose_create_state      = 0x006ee5a0;
   constexpr uintptr_t shading_state_get_int_param    = 0x006ee6c0;

   // ---- Entity / Droideka death animation fix -----------------------------------

   constexpr uintptr_t droideka_update_nextstate_call = 0x004a4b94;
   constexpr uintptr_t entity_droideka_rtti_hash      = 0x01ebd06c;
   constexpr uintptr_t lowres_prone_anim_name_ptr     = 0x007ea9e0;
   constexpr uintptr_t lowres_prone_case_imm          = 0x0064a26a;

   // ---- Entity / Vehicle (Carrier/Flyer) ----------------------------------------

   constexpr uintptr_t carrier_vtable                 = 0x0079b2ec;
   constexpr uintptr_t carrier_update                 = 0x004971d0;
   constexpr uintptr_t carrier_kill                   = 0x00497110;
   constexpr uintptr_t flyer_render                   = 0x004ab040;

   // ---- Debug / Visualization ---------------------------------------------------

   constexpr uintptr_t freecam_update                 = 0x0052d7b0;

   // ---- Vehicle view toggle (FP/TP) ---------------------------------------------

   constexpr uintptr_t veh_view_hover_vtable_3c_slot  = 0x0079cb80;
   constexpr uintptr_t veh_view_walker_vtable_3c_slot = 0x0079ea64;
   constexpr uintptr_t veh_view_cmd_hover_vtable_3c_slot = 0x007993f4;
   constexpr uintptr_t veh_view_cmd_walker_vtable_3c_slot = 0x007997bc;
   // The two "original" slot thunks are read straight out of the GOG vtable
   // slots above, which is authoritative: both hold B0 01 C3 (MOV AL,1; RET),
   // and return_false_thunk holds 32 C0 C3 (XOR AL,AL; RET), same as Steam.
   constexpr uintptr_t veh_view_hover_3c_orig_thunk   = 0x00478db0;
   constexpr uintptr_t veh_view_walker_3c_orig_thunk  = 0x0047fc70;
   constexpr uintptr_t veh_view_return_false_thunk    = 0x004774c0;

   // ---- Weapon / Grappling Hook -------------------------------------------------

   constexpr uintptr_t grapple_dtor                   = 0x00600400;

   // ---- Animation (weapon/soldier) ----------------------------------------------

   constexpr uintptr_t set_weapon_anim_map            = 0x00640850;
   constexpr uintptr_t anim_add_bank                  = 0x00520ec0;

   // ---- Animation bank appending (anim_bank_append.cpp) -------------------------

   constexpr uintptr_t anim_finder_resolve            = 0x00646080;
   constexpr uintptr_t anim_class_find_in_banks       = 0x00645340;
   constexpr uintptr_t anim_hash_table                = 0x0099fcac;
   constexpr uintptr_t zephyr_anim_bank_find          = 0x0072cb10;

   // ---- Flyer boost animation ---------------------------------------------------

   constexpr uintptr_t flyer_init_animations          = 0x004b9720;

   // ---- Carrier turret fire -----------------------------------------------------

   constexpr uintptr_t turret_fire_check              = 0x005a748f;
   constexpr uintptr_t turret_fire_allow              = 0x005a74a1;
   constexpr uintptr_t turret_fire_block              = 0x005a74ca;
   constexpr uintptr_t turret_update_indirect         = 0x005a9280;
   constexpr uintptr_t trigger_update                 = 0x0043a940;
   constexpr uintptr_t zephyr_pose_dyn_set_time       = 0x0072e720;
   constexpr uintptr_t g_identity_matrix              = 0x009cc380;

   // ---- Hashing (thiscall wrapper) ----------------------------------------------

   constexpr uintptr_t hash_string_thiscall           = 0x00727df0;

   // ---- Debug / Logging ---------------------------------------------------------

   constexpr uintptr_t game_log                       = 0x006f80c0;
   // Both confirmed by decompile 2026-07-27.  SetLogData matches Steam's body
   // field-for-field (same order, same g_bFormatted=1, same NULL fallbacks);
   // GetContentDirectory sits at the SAME address as Steam.
   constexpr uintptr_t red_warning_set_log_data       = 0x006F8270;
   constexpr uintptr_t log_formatted_flag             = 0x009C9930;
   constexpr uintptr_t dlc_get_content_directory      = 0x0048EBA0;

   // ---- Shell / In-game movies --------------------------------------------------
   // See the modtools namespace.  Reg table 0x007E8058; StartInGameMoviePlay is
   // 0x00535940 (instruction-for-instruction the same as Steam's 0x00534BD0).
   constexpr uintptr_t scriptcb_play_ingame_movie     = 0x00586520;
   constexpr uintptr_t ingame_movie_filename          = 0x01E57738;
   constexpr uintptr_t ingame_movie_player_state      = 0x01E5761C;

   // ---- Shell / GC Visual Limits ------------------------------------------------

   constexpr uintptr_t gc_beam_add                    = 0x00580cf0;
   constexpr uintptr_t gc_particle_add                = 0x00581050;
   constexpr uintptr_t gc_beam_count_patches[]      = {
       0x00580cfc, 0x00580d19, 0x00580950, 0x00580cba, 0x00580cc8, 0x00580cd9, 0x0058132e,
   };
   constexpr uintptr_t gc_particle_count_patches[]  = {
       0x00581062, 0x0058108c, 0x00580f52, 0x00581029, 0x00581037, 0x005812da,
   };
   constexpr uintptr_t gc_beam_limit_imm8_op          = 0x00580d02;
   constexpr uintptr_t gc_particle_limit_imm32_op     = 0x00581068;
   constexpr uintptr_t gc_particle_alloc_size_op      = 0x005812a5;
   constexpr uintptr_t gc_beam_alloc_size_op          = 0x005812f3;

   // ---- RedParticleRenderer (see modtools namespace for docs) -------------------

   constexpr uintptr_t rpr_submit_particle            = 0x006d4440;
   constexpr uintptr_t rpr_current_cache              = 0x00967654;
   constexpr uintptr_t rpr_cache_index                = 0x00967658;
   constexpr uintptr_t rpr_setcache_base_operand      = 0x006d43a6;
   constexpr uintptr_t rpr_setcache_limit_imm8_op     = 0x006d438a;

   // ---- Aim Assist --------------------------------------------------------------

   constexpr uintptr_t anim_finder_add_bank           = 0x00646aa0;
   constexpr uintptr_t carrier_set_property           = 0x004976b0;
   constexpr uintptr_t carrier_attach_cargo           = 0x00497300;
   constexpr uintptr_t carrier_update_spawn           = 0x00670410;
   constexpr uintptr_t vehicle_tracker_pool           = 0x01f9b720;
   constexpr uintptr_t gameloop_pause_mode            = 0x01e574f6;
   constexpr uintptr_t carrier_take_off               = 0x004b3c60;
   constexpr uintptr_t cloth_enforce_collisions       = 0x0045ba40;
   constexpr uintptr_t cloth_satisfy_constraints      = 0x0045bc40;
   constexpr uintptr_t cloth_enforce_cylinder_coll    = 0x0045b7c0;
   constexpr uintptr_t load_config_real               = 0x00578560;
   constexpr uintptr_t load_data_file_real            = 0x005783a0;
   constexpr uintptr_t load_end_real                  = 0x00577910;
   constexpr uintptr_t mem_pool_alloc                 = 0x006dd410;
   constexpr uintptr_t pbl_read_next_data             = 0x00728f00;
   constexpr uintptr_t pbl_read_next_scope            = 0x00728f80;
   constexpr uintptr_t progress_set_all_on            = 0x00579980;
   constexpr uintptr_t GameSound_play                 = 0x00538d80;
   constexpr uintptr_t red_pose_convert_skel32        = 0x006debd0;
   constexpr uintptr_t snd_engine_update              = 0x00735680;
   constexpr uintptr_t snd_soundstream_init           = 0x00737c30;
   constexpr uintptr_t snd_stream_slot_count_imm8     = 0x0073518c;
   constexpr uintptr_t snd_engine_get_free_stream     = 0x00735170;
   constexpr uintptr_t zephyr_pose_dyn_set_anim       = 0x0072e500;
   constexpr uintptr_t zephyr_pose_static_ctor        = 0x0072eb60;
   constexpr uintptr_t zephyr_pose_static_open        = 0x0072eff0;
   constexpr uintptr_t zephyr_skeleton_open           = 0x0072dd10;
   constexpr uintptr_t anim_add_skeleton_bank         = 0x00645610;
   constexpr uintptr_t carrier_initiate_landing       = 0x004b3d50;
   constexpr uintptr_t draw_line_3d                   = 0x006f2360;
   constexpr uintptr_t draw_sphere                    = 0x006f1b30;
   constexpr uintptr_t pbl_config_copy_ctor           = 0x00728eb0;
   constexpr uintptr_t spline_build                   = 0x0072b7f0;
   constexpr uintptr_t zephyr_pose_static_blend       = 0x0072ed50;
   constexpr uintptr_t zephyr_pose_static_dtor        = 0x0072ebe0;
   constexpr uintptr_t zephyr_pose_static_set         = 0x0072f0a0;
   constexpr uintptr_t zephyr_skeleton_finalize       = 0x0072cd00;
   constexpr uintptr_t anim_find_animation            = 0x00520f50;
   constexpr uintptr_t carrier_detach_cargo           = 0x00497410;
   constexpr uintptr_t console_add_variable           = 0x0041fe50;
   constexpr uintptr_t disguise_raise                 = 0x00683fa0;
   constexpr uintptr_t load_update_real               = 0x00577980;
   constexpr uintptr_t weapon_signal_fire             = 0x0067a6b0;
   constexpr uintptr_t snd_sound_play                 = 0x0073b510;
   constexpr uintptr_t passenger_activate             = 0x00610b50;
   constexpr uintptr_t turret_activate                = 0x005a8b20;
   constexpr uintptr_t rumble_state_setup             = 0x006b3820;

   // ---- Flyer path following / engine sound -------------------------------------

   constexpr uintptr_t aimer_activate                 = 0x0043e370;
   constexpr uintptr_t get_weapon_anim_map            = 0x0063da10;
   // Same wrong-sibling mistake as steam had, one shift over: 0x00737b80 walks
   // 0x007e4584, the list Sound::Play never touches. The Sound one walks
   // 0x007e46f8 and is byte-identical to steam 0x00739d90. See the steam entry.
   constexpr uintptr_t snd_find_by_hash_id            = 0x0073ae70;

   // ---- Snd::Properties field offsets (NOT addresses) ---------------------------
   // Release layout, identical to steam; see the steam namespace for how they
   // were derived. Without these the table falls back to the modtools values
   // (0x1c / 0x68), which are 4 too high for a release build.
   constexpr uintptr_t snd_props_loop_byte            = 0x18;
   constexpr uintptr_t snd_props_next_allowed         = 0x64;

   // ---- Loading screen + Snd voice ownership (ported 2026-07-28) ----------------
   // tools/port_gog.py auto, every one score 1.00 on the lockstep disassembly
   // compare (or 3+ agreeing reference sites for the globals). These were the
   // last gap keeping GOG out of the loading-screen module's all-or-nothing
   // install gate; with pbl_config_ctor present it now runs there too.
   //
   // Five of them cross-confirm each other in one decompile: LoadDataChunk
   // 0x00578460 calls 0x0072bb10 (ReadNextChild) then dispatches 'modl' to
   // 0x006c5480, 'tex_' to 0x006bb360, 'skel' to 0x006e9960 and 'load' to
   // 0x00578560 (= load_config_real, already in this table).
   constexpr uintptr_t load_data_chunk_real           = 0x00578460;
   constexpr uintptr_t pbl_chunk_read_next_child      = 0x0072bb10;
   constexpr uintptr_t red_model_read                 = 0x006c5480;
   // Red3DModelElementLite::SetModel — port_gog.py from steam 0x006d7010,
   // score 1.00 (every instruction matched), shift +0x10a0 in-run.
   constexpr uintptr_t model_elem_set_model           = 0x006d80b0;
   constexpr uintptr_t red_texture_read               = 0x006bb360;
   constexpr uintptr_t red_skeleton_read              = 0x006e9960;
   // PblConfig::PblConfig — byte-identical to steam 0x00727da0, and adjacent to
   // pbl_config_copy_ctor 0x00728eb0 exactly as the pair sits on steam.
   constexpr uintptr_t pbl_config_ctor                = 0x00728e70;
   constexpr uintptr_t load_update_qpc_stamp          = 0x01fabf20;
   constexpr uintptr_t s_loadheap_global              = 0x01f9d798;
   constexpr uintptr_t runtime_heap_global            = 0x01e57610;
   // Snd::Sound::VoiceVirtualToVoiceVirtualHandle — (voice - smVoiceVirtuals
   // 0x01e2c960) / 200 + 1, same reciprocal-multiply body as steam 0x0073afb0.
   constexpr uintptr_t voice_to_handle                = 0x0073c0a0;
   constexpr uintptr_t voice_virtual_release          = 0x005393a0;
   constexpr uintptr_t gamesound_controllable_stop    = 0x005393d0;
   constexpr uintptr_t gamesound_stolen_callback      = 0x005394a0;

   constexpr uintptr_t carrier_update_landed_ht       = 0x004974b0;
   constexpr uintptr_t disguise_drop                  = 0x00684100;
   constexpr uintptr_t load_render_real               = 0x00577c90;
   constexpr uintptr_t render_screen_real             = 0x00578000;
   constexpr uintptr_t platform_render_texture        = 0x00423950;
   constexpr uintptr_t disguise_set_property          = 0x006844a0;
   constexpr uintptr_t game_model_table               = 0x01ec26b4;

   // ---- Lua core / character system (ported 2026-07-20) -------------------------

   constexpr uintptr_t char_exit_vehicle              = 0x004f1380;
   constexpr uintptr_t char_array_base                = 0x01e317d4;
   constexpr uintptr_t max_chars                      = 0x01e317d0;
   constexpr uintptr_t team_array_base                = 0x007eaaa0;
   constexpr uintptr_t class_def_list                 = 0x007ed4f0;
   constexpr uintptr_t aimer_set_weapon               = 0x0043e3f0;
   constexpr uintptr_t lua_create_entity              = 0x0058fac0;
   constexpr uintptr_t enter_state_path_op            = 0x005783e1;
   constexpr uintptr_t net_in_shell                   = 0x007e9007;
   constexpr uintptr_t net_enabled                    = 0x01e64359;
   constexpr uintptr_t net_enabled_next               = 0x01e64358;

   // ---- First person (from FirstPerson::Init 0x521000) --------------------------

   constexpr uintptr_t fp_renderable                  = 0x01e573a8;
   constexpr uintptr_t fp_anim_array                  = 0x01e572e0;
   constexpr uintptr_t anim_name_table                = 0x0078a710;
   constexpr uintptr_t fp_update_soldier              = 0x0051fb70;

   // ---- Fog (see modtools namespace for docs) -----------------------------------

   constexpr uintptr_t red_renderer_set_fog_range     = 0x006b46d0;
   constexpr uintptr_t red_renderer_set_fog_enable    = 0x006b46b0;
   constexpr uintptr_t fl_fog_start                   = 0x008f825c;
   constexpr uintptr_t fl_fog_end                     = 0x008f8260;

   // ---- Combat awards (see modtools namespace for docs) -------------------------
   // port_gog.py code 0x5a33d0 / 0x5a3350 -> score 1.00, shift +0xfb0 (in-run)
   constexpr uintptr_t medals_is_award_available      = 0x005a4380;
   constexpr uintptr_t medals_is_award_available_internal = 0x005a4300;

} // namespace gog

} // namespace game_addrs
