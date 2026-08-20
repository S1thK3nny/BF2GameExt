// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

#include "apply_patches.hpp"
#include "resolve.hpp"
#include "lua/lua_hooks.hpp"
#include "controller/controller_support.hpp"
#include "controller/controller_rumble.hpp"
#include "controller/aim_assist.hpp"
#include "entity/anim_bank_append.hpp"
#include "weapon/disguise_model_override.hpp"
#include "weapon/barrel_fire_origin.hpp"
#include "entity/land_on_arrival_fix.hpp"
#include "entity/droideka_ball_mode.hpp"
#include "entity/soldier_override_texture.hpp"
#include "entity/vehicle_view_toggle.hpp"
#include "ai/ai_fairness.hpp"
#include "entity/cloth_collision_fix.hpp"
#include "entity/droideka_death_anim_fix.hpp"
#include "entity/award_disable.hpp"
#include "entity/flyer_sound_fix.hpp"
#include "entity/soldier_prone.hpp"
#include "entity/prone_lvl_load.hpp"
#include "entity/terrain_texture_fix.hpp"
#include "entity/hover_pilot_null_fix.hpp"
#include "entity/ai_squad_order_null_fix.hpp"
#include "entity/hero_team_switch_fix.hpp"
#include "entity/command_post_null_fix.hpp"
#include "entity/branch_region_debug.hpp"
#include "entity/branch_region_fix.hpp"
#include "util/sound_diag.hpp"
#include "util/voice_limit.hpp"
#include "entity/jetpack_fp_sound_fix.hpp"
#include "render/blur_downsize_clamp.hpp"
#include "render/screenshot_fix.hpp"
#include "render/hud_widescreen.hpp"
#include "render/red_light_stale_node_fix.hpp"
#include "render/water_texture_count_fix.hpp"
#include "render/particle_batch_spill.hpp"
#include "render/particle_density.hpp"
#include "weapon/anim_textures.hpp"
#include "weapon/lightsaber_illumination.hpp"
#include "shell/dlc_mission_init_fix.hpp"
#include "shell/map_queue_fix.hpp"
#include "shell/gc_visual_limits.hpp"
#include "util/crash_logger.hpp"
#include "util/audio_stream_limit.hpp"
#include "util/enable_sound_warnings.hpp"
#include "util/error_dialog_fix.hpp"
#include "util/game_logging.hpp"
#include "util/ini_config.hpp"
#include "util/slim_vector.hpp"

static bool g_initialized = false;

static void install_patches_impl(uintptr_t exe_base, const char* ini_path);

// ---------------------------------------------------------------------------
// CombatHelper::DeadBodyCheck (0x5b4ec0) toggles.
//
// Vanilla behaviour: Alliance units (Team::mSide == 1) walk to and shoot
// nearby soldier corpses. Two optional byte patches control this. They are
// modtools VAs and are guarded by their expected original bytes, so they
// silently no-op on the GOG/Steam builds (different addresses).
//
//   disableAll  — NOP the first guard's JGE so DeadBodyCheck always returns
//                 false: NOBODY shoots corpses (this overrides allFactions,
//                 since the function bails before reaching the side gate).
//                   0x5b4ed3: 7D 07  JGE 0x5b4edc -> 90 90
//                   (falls through to the 0x5b4ed5 XOR AL,AL / RET epilogue)
//
//   allFactions — NOP the mSide==1 JNZ so the side gate always passes: ALL
//                 factions shoot corpses (the team != 0 null-check above is
//                 left intact).
//                   0x5b4f06: 75 16  JNZ 0x5b4f1e -> 90 90
//
// Must run while the executable sections are still RW (before re-protect).
// ---------------------------------------------------------------------------
static void apply_deadbody_check_patches(uintptr_t exe_base, bool disableAll, bool allFactions)
{
   if (disableAll) {
      uint8_t* p = (uint8_t*)resolve(exe_base, 0x5b4ed3);
      if (p[0] == 0x7D && p[1] == 0x07) { p[0] = 0x90; p[1] = 0x90; }
   }
   if (allFactions) {
      uint8_t* p = (uint8_t*)resolve(exe_base, 0x5b4f06);
      if (p[0] == 0x75 && p[1] == 0x16) { p[0] = 0x90; p[1] = 0x90; }
   }
}

// ---------------------------------------------------------------------------
// Proxy path: BF2GameExt_Init / BF2GameExt_Shutdown
// Called by DInput8Proxy after LoadLibrary. The proxy sets the
// BF2GAMEEXT_PROXY env var before loading us, so DllMain skips auto-init.
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) BOOL WINAPI BF2GameExt_Init(uintptr_t exe_base, const char* ini_path)
{
   if (g_initialized) return TRUE;
   install_patches_impl(exe_base, ini_path);
   return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI BF2GameExt_Shutdown()
{
   if (!g_initialized) return;
   lua_hooks_uninstall();
   g_initialized = false;
}

// ---------------------------------------------------------------------------
// Exe patcher path: DllMain auto-init (no INI, no proxy)
// ---------------------------------------------------------------------------

BOOL __declspec(dllexport) APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   switch (ul_reason_for_call) {
   case DLL_PROCESS_ATTACH: {
      // If the proxy loaded us, it will call BF2GameExt_Init explicitly.
      // Skip auto-init so the INI config is respected.
      char buf[2];
      if (GetEnvironmentVariableA("BF2GAMEEXT_PROXY", buf, sizeof(buf)) > 0)
         break;

      uintptr_t exe_base = (uintptr_t)GetModuleHandleW(nullptr);
      install_patches_impl(exe_base, nullptr);
   } break;
   case DLL_THREAD_ATTACH:
   case DLL_THREAD_DETACH:
      break;
   case DLL_PROCESS_DETACH:
      if (g_initialized) {
         lua_hooks_uninstall();
         g_initialized = false;
      }
      break;
   }
   return TRUE;
}

void __declspec(dllexport) ExportFunction() {}

// ---------------------------------------------------------------------------
// Shared init logic
// ---------------------------------------------------------------------------

static void install_patches_impl(uintptr_t exe_base, const char* ini_path)
{
   char* const game_address = (char*)exe_base;

   IMAGE_DOS_HEADER& dos_header = *(IMAGE_DOS_HEADER*)game_address;
   IMAGE_NT_HEADERS32& nt_headers = *(IMAGE_NT_HEADERS32*)(game_address + dos_header.e_lfanew);
   IMAGE_FILE_HEADER& file_header = nt_headers.FileHeader;
   IMAGE_OPTIONAL_HEADER32& optional_header = nt_headers.OptionalHeader;

   assert(dos_header.e_magic == 'ZM');
   assert(nt_headers.Signature == 'EP');
   assert(optional_header.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC);

   const size_t section_headers_offset =
      dos_header.e_lfanew + (sizeof(IMAGE_NT_HEADERS32) - sizeof(IMAGE_OPTIONAL_HEADER32)) +
      file_header.SizeOfOptionalHeader;

   IMAGE_SECTION_HEADER* section_headers = (IMAGE_SECTION_HEADER*)(game_address + section_headers_offset);

   slim_vector<section_info> sections{file_header.NumberOfSections,
                                      slim_vector<section_info>::alloc_tag{}};

   for (int i = 0; i < file_header.NumberOfSections; ++i) {
      sections[i] = {
         .memory_start = game_address + section_headers[i].VirtualAddress,
         .file_start = section_headers[i].PointerToRawData,
         .file_end = section_headers[i].PointerToRawData + section_headers[i].SizeOfRawData,
      };
   }

   // Are we even in the game? Our dinput8.dll proxy sits in GameData, so any
   // program started from that folder that touches DirectInput loads us too --
   // the Battlefront II Mod Loader being the one people hit. Bail on a foreign
   // process before anything with a side effect: patching someone else's image
   // is meaningless, FatalAppExit'ing it kills their program, and opening the
   // log "w" from a launcher wipes the log from the last real game session.
   if (identify_exe(exe_base, sections) == exe_identity::foreign) return;

   // Before any patching: capture first-chance fatal exceptions to
   // BF2GameExt_crash.log (the game's own SEH handler can otherwise swallow the
   // crash and exit without a trace).
   crash_logger_install();

   slim_vector<DWORD> section_protection_values{file_header.NumberOfSections,
                                                slim_vector<DWORD>::alloc_tag{}};

   for (int i = 0; i < file_header.NumberOfSections; ++i) {
      if (not VirtualProtect(game_address + section_headers[i].VirtualAddress,
                             section_headers[i].Misc.VirtualSize, PAGE_READWRITE,
                             &section_protection_values[i])) {
         FatalAppExitA(0, "Failed to make executable sections writable!");
      }
   }

   if (not apply_patches(exe_base, sections, ini_path)) {
      FatalAppExitA(0, "Failed to apply patches! Check \"BF2GameExt.log\" for more info.");
   }

   // Read INI toggles before installing hooks (some hooks check config at install time).
   // Defaults: dead-body shooting disabled for everyone; all-factions toggle off.
   bool disableDeadBody       = true;
   bool deadBodyAllFactions   = false;
   if (ini_path) {
      ini_config cfg{ini_path};
      g_useBarrelFireOrigin = cfg.get_bool("Fixes", "BarrelFireOriginFix", true);
      g_proneEnabled = cfg.get_bool("Features", "Prone", true);
      g_gameLoggingEnabled = cfg.get_bool("Features", "GameLogging", false);
      g_enableSoundWarnings = cfg.get_bool("Features", "EnableSoundWarnings", false);
      g_terrainTextureFixEnabled = cfg.get_bool("Fixes", "TerrainTextureFix", true);
      g_blurDownsizeClampEnabled = cfg.get_bool("Fixes", "BlurDownsizeClamp", true);
      g_screenshotFixEnabled = cfg.get_bool("Fixes", "ScreenshotFix", true);
      g_aiPlayerVisionFairness   = cfg.get_bool("AI", "PlayerVisionFairness", true);
      g_aiPlayerPriorityFairness = cfg.get_bool("AI", "PlayerPriorityFairness", true);
      g_aiPlayerThreatFairness   = cfg.get_bool("AI", "PlayerThreatFairness", false);
      g_aiPlayerAwarenessFairness = cfg.get_bool("AI", "PlayerAwarenessFairness", true);
      g_errorDialogFixEnabled = cfg.get_bool("Fixes", "ErrorDialogFix", true);
      g_dlcMissionInitFixEnabled = cfg.get_bool("Fixes", "DLCMissionInitFix", false);
      g_gcVisualLimitsEnabled = cfg.get_bool("LimitIncreases", "GCVisualLimits", true);
      g_particleBatchSpillEnabled = cfg.get_bool("Particles", "ParticleFixes", true);
      g_particleDensity           = cfg.get_int("Particles", "ParticleDensity", 0);
      g_commandPostNullFixEnabled = cfg.get_bool("Fixes", "CommandPostNullFix", true);
      g_branchRegionDebugEnabled  = cfg.get_bool("Fixes", "BranchRegionDebug", false);
      g_branchRegionFixEnabled    = cfg.get_bool("Fixes", "BranchRegionFix", true);
      g_soundDiagEnabled          = cfg.get_bool("Fixes", "SoundDiagnostic", false);
      g_voiceLimit                = cfg.get_int("LimitIncreases", "VoiceLimit", 0);
      g_droidekaDeathAnimEnabled = cfg.get_bool("Fixes", "DroidekaDeathAnimation", true);
      g_disableAwardBuffs = cfg.get_bool("Features", "DisableAwardBuffs", false);
      g_disableAwardWeapons = cfg.get_bool("Features", "DisableAwardWeapons", false);
      g_lightsaberIlluminationEnabled = cfg.get_bool("Lightsaber", "LightsaberIllumination", true);
      g_lightsaberLightRadius = cfg.get_float("Lightsaber", "LightsaberLightRadius", 4.0f);
      g_lightsaberLightIntensity = cfg.get_float("Lightsaber", "LightsaberLightIntensity", 1.0f);
      g_reticleCorrection = cfg.get_float("Fixes", "ReticleCorrection", -1.0f);
      g_controllerEnabled = cfg.get_bool("Controller", "Enabled", true);
      g_rumbleEnabled = g_controllerEnabled && cfg.get_bool("Controller", "Rumble", true);
      disableDeadBody     = cfg.get_bool("Features", "DisableDeadBodyShooting", true);
      deadBodyAllFactions = cfg.get_bool("Features", "DeadBodyShootingAllFactions", false);
      controller_set_ini_path(ini_path);
      aim_assist_load_config(ini_path);
   } else {
      g_useBarrelFireOrigin = true;
      g_proneEnabled = true;
      g_controllerEnabled = true;
      g_rumbleEnabled = true;
   }

   // Sections are still RW here (re-protected below) — safe to apply byte patches.
   apply_deadbody_check_patches(exe_base, disableDeadBody, deadBodyAllFactions);

   // Resolve Lua API addresses and register our custom functions into the live Lua state.
   lua_hooks_install(exe_base);

   // Installers that select their address set from g_addr; each no-ops where its
   // addresses are unknown.  Called here while sections are still RW.
   barrel_fire_origin_install(exe_base);
   aim_assist_install(exe_base);
   prone_system_install(exe_base);
   prone_lvl_load_install(exe_base); // must follow prone_system_install — owns g_proneEnabled
   anim_bank_append_install(exe_base);
   disguise_ext_install(exe_base);
   game_logging_install(exe_base);
   terrain_texture_fix_install(exe_base);
   blur_downsize_clamp_install(exe_base);
   screenshot_fix_install(exe_base);
   water_texture_count_fix_install(exe_base); // byte-patches .text — needs the RW window
   error_dialog_fix_install(exe_base); // byte-patches .text — needs the RW window
   dlc_mission_init_fix_install(exe_base);
   map_queue_fix_install(exe_base);    // byte-patches .text — needs the RW window
   // Before gc_visual_limits_install: that one's log line reports the live
   // cache-slot count, and the GC beam path depends on the spill being up.
   particle_density_install(exe_base);     // byte-patches .text/.rdata
   particle_batch_spill_install(exe_base); // byte-patches .text — needs the RW window
   gc_visual_limits_install(exe_base); // byte-patches .text — needs the RW window
   hud_widescreen_install(exe_base);   // byte-patches .text — needs the RW window
   anim_textures_install(exe_base);
   land_on_arrival_install(exe_base);  // byte-patches .text — needs the RW window
   hover_pilot_null_fix_install(exe_base); // byte-patches .text — needs the RW window
   ai_squad_order_null_fix_install(exe_base); // byte-patches .text — needs the RW window
   hero_team_switch_fix_install(exe_base);    // byte-patches .text — needs the RW window
   command_post_null_fix_install(exe_base);
   branch_region_fix_install(exe_base);
   branch_region_debug_install(exe_base);
   jetpack_fp_sound_fix_install(exe_base);   // byte-patches .text — needs the RW window
   flyer_sound_install(exe_base);
   enable_sound_warnings_install(exe_base);
   audio_stream_limit_install(exe_base);
   voice_limit_install(exe_base);  // byte-patches .text/.data - needs the RW window
   sound_diag_install(exe_base);
   droideka_ball_mode_install(exe_base);
   droideka_death_anim_install(exe_base); // byte-patches .text — needs the RW window
   award_disable_install(exe_base);
   soldier_override_texture_install(exe_base);
   vehicle_view_toggle_install(exe_base); // vtable-slot patches — needs the RW window
   cloth_collision_fix_install(exe_base);
   ai_fairness_install(exe_base);      // byte-patches .text — needs the RW window
   // Before the saber lights: its uninstall deactivates our own lights, and those
   // calls should go through the guard.
   red_light_stale_node_fix_install(exe_base);
   lightsaber_illumination_install(exe_base);

   for (int i = 0; i < file_header.NumberOfSections; ++i) {
      if (not VirtualProtect(game_address + section_headers[i].VirtualAddress,
                             section_headers[i].Misc.VirtualSize, section_protection_values[i],
                             &section_protection_values[i])) {
         FatalAppExitA(0, "Failed to restore normal executable sections virtual protect!");
      }
   }

   g_initialized = true;
}
