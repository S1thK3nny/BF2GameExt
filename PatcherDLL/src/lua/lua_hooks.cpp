#include "pch.h"
#include "lua_hooks.hpp"
#include "lua_funcs.hpp"
#include "core/game_addrs.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/game_logging.hpp"
#include "entity/terrain_texture_fix.hpp"
#include "loading_screen/loading_screen.hpp"
#include "entity/flyer_carrier_fixes.hpp"
#include "entity/soldier_prone.hpp"
#include "entity/vehicle_view_toggle.hpp"
#include "entity/soldier_fp_animation_override.hpp"
#include "entity/droideka_ball_mode.hpp"
#include "entity/soldier_override_texture.hpp"
#include "entity/droideka_death_anim_fix.hpp"
#include "entity/flyer_boost_animation.hpp"
#include "entity/cloth_collision_fix.hpp"
#include "weapon/disguise_model_override.hpp"
#include "weapon/grappling_hook.hpp"
#include "weapon/barrel_fire_origin.hpp"
#include "debug_commands/command_registry.hpp"
#include "shell/gc_visual_limits.hpp"
#include "render/particle_batch_spill.hpp"
#include "render/particle_density.hpp"
#include "entity/command_post_null_fix.hpp"
#include "entity/branch_region_debug.hpp"
#include "entity/branch_region_fix.hpp"
#include "shell/ingame_movie_path.hpp"
#include "entity/anim_bank_append.hpp"
#include "entity/land_on_arrival_fix.hpp"
#include "entity/flyer_sound_fix.hpp"
#include "weapon/shield_channel_fix.hpp"
#include "weapon/lightsaber_illumination.hpp"
#include "render/red_light_stale_node_fix.hpp"
#include "render/water_texture_count_fix.hpp"
#include "controller/controller_support.hpp"
#include "controller/controller_rumble.hpp"
#include "controller/aim_assist.hpp"

#include <detours.h>

lua_api g_lua = {};
lua_State* g_L = nullptr;
char g_loadDisplayPath[260] = "Load\\load";

// ---------------------------------------------------------------------------
// OnCharacterExitVehicle event system
//
// Fully custom callback storage + dispatch.  Registration functions
// (OnCharacterExitVehicle, Name/Team/Class, Release) are Lua C functions
// in lua_funcs.cpp that store callbacks in the Lua registry and track
// filter metadata in the g_cevCallbacks[] array below.
//
// The C++ Detours hook on the exit-vehicle function (0x0052FC70) scans the
// character array to find charIndex + vehicleCtrl, resolves the character's
// name/team/class, then fires all matching callbacks via rawgeti + pcall.
//
// Lua usage (identical to vanilla On-Events):
//   local h = OnCharacterExitVehicle(function(player, vehicle) ... end)
//   ReleaseCharacterExitVehicle(h)
//   h = nil
// ---------------------------------------------------------------------------

CEVCallback g_cevCallbacks[CEV_MAX_CBS] = {};
int g_cevNextKey = -1000;

// __fastcall mirrors __thiscall ABI: ECX=this, EDX=unused, then stack args.
using fn_char_exit_vehicle = void(__fastcall*)(void* ecx, void* edx_unused, int arg1, int arg2);
static fn_char_exit_vehicle original_char_exit_vehicle = nullptr;



static void __fastcall hooked_char_exit_vehicle(void* thisPtr, void* /*edx*/, int arg1, int arg2)
{
   // thisPtr = character Controllable* (EntitySoldier + 0x240; base invariant
   // across builds).  Character slot layout (stride 0x1B0, +0x148 ctrl, +0x14C
   // vehicle ctrl, +0x134 team) is build-invariant — verified on Steam via
   // Lua_Callbacks::GetCharacterUnit/GetCharacterTeam.
   // Scan the character array BEFORE calling original (original clears state).
   int       charIndex      = -1;
   void*     vehicleCtrl    = nullptr;
   int       charTeam       = -1;
   uint32_t  entityNameHash = 0;
   void*     entityClassPtr = nullptr;

   const uintptr_t exe_base = (uintptr_t)GetModuleHandleW(nullptr);
   auto res = [=](uintptr_t a) -> uintptr_t { return a - kUnrelocatedBase + exe_base; };

   __try {
      const uintptr_t arrayBase = *(uintptr_t*)res(g_addr->char_array_base);
      const int       maxChars  = *(int*)      res(g_addr->max_chars);
      if (arrayBase && maxChars > 0) {
         for (int i = 0; i < maxChars; i++) {
            const uintptr_t slot = arrayBase + (uintptr_t)i * 0x1B0;
            if (*(void**)(slot + 0x148) == thisPtr) {
               charIndex    = i;
               vehicleCtrl  = *(void**)(slot + 0x14C);
               charTeam     = *(int*)(slot + 0x134);

               char* entitySoldier = (char*)thisPtr - 0x240;
               entityNameHash  = *(uint32_t*)(entitySoldier + 4);   // EntityEx::mId
               entityClassPtr  = *(void**)   (entitySoldier + 8);   // EntityEx::mEntityClass
               break;
            }
         }
      }
   }
   __except (EXCEPTION_EXECUTE_HANDLER) {}

   // vehicleEntity = vehicleCtrl - 0x240 (EntitySoldier base)
   void* vehicleEntity = vehicleCtrl ? (char*)vehicleCtrl - 0x240 : nullptr;

   // Fire all matching callbacks.
   if (charIndex >= 0 && g_L) {
      for (int i = 0; i < CEV_MAX_CBS; i++) {
         if (g_cevCallbacks[i].regKey == 0) continue;

         bool match = false;
         switch (g_cevCallbacks[i].filterType) {
            case CEV_PLAIN: match = true; break;
            case CEV_NAME:  match = (entityNameHash != 0) && (entityNameHash == g_cevCallbacks[i].nameHash); break;
            case CEV_TEAM:  match = (charTeam == g_cevCallbacks[i].teamFilter); break;
            case CEV_CLASS: match = (entityClassPtr != nullptr) && (entityClassPtr == g_cevCallbacks[i].classPtr); break;
         }

         if (match) {
            __try {
               g_lua.rawgeti(g_L, -10001, g_cevCallbacks[i].regKey);
               g_lua.pushnumber(g_L, (float)charIndex);
               g_lua.pushlightuserdata(g_L, vehicleEntity);
               int rc = g_lua.pcall(g_L, 2, 0, 0);
               if (rc != 0 && g_lua.tolstring) {
                  size_t len = 0;
                  const char* err = g_lua.tolstring(g_L, -1, &len);
                  if (err) get_gamelog()("[CEV] pcall error: %s\n", err);
                  g_lua.settop(g_L, -2);
               }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
         }
      }
   }

   original_char_exit_vehicle(thisPtr, nullptr, arg1, arg2);
}

// ---------------------------------------------------------------------------
// LoadDisplay::EnterState hook
//
// The original function has a hardcoded PUSH imm32 pointing at "Load\\load"
// in .rdata (VA 0x00A5AF5C). During install we patch the 4-byte operand of
// that PUSH instruction to point to g_loadDisplayPath instead.  Modders call
// SetLoadDisplayLevel() from ScriptPreInit to override the path before the
// load screen fires.
// ---------------------------------------------------------------------------


// Saved so we can restore on uninstall.
static uint32_t* g_enter_state_path_op_ptr  = nullptr;
static uint32_t  g_enter_state_path_op_orig = 0;

using fn_init_state = void(__cdecl*)();
static fn_init_state original_init_state = nullptr;

static void __cdecl hooked_init_state()
{
   original_init_state();

   // RESET the path to vanilla every time a new mission/state starts
   strncpy_s(g_loadDisplayPath, sizeof(g_loadDisplayPath), "Load\\load", _TRUNCATE);

   g_L = *(lua_State**)resolve((uintptr_t)GetModuleHandleW(nullptr), g_addr->g_lua_state_ptr);

   // Reset callback storage for the new Lua state.
   memset(g_cevCallbacks, 0, sizeof(g_cevCallbacks));
   g_cevNextKey = -1000;

   // Reset FP animation bank mappings (stale class pointers from previous level)
   fp_anim_bank_reset();
   flyer_boost_anim_reset();
   disguise_ext_reset();
   droideka_ball_mode_reset();
   soldier_override_texture_reset();
   freecam_light_reset(); // its pool block did not survive the level change

   if (g_build == GameBuild::Modtools) {
      // Register debug console commands (engine is fully initialized now).
      // Console registration addresses are modtools-only.
      DebugCommandRegistry::lateInit();
   }

   if (g_L) {
      register_lua_functions(g_L);
   }

   if (g_build != GameBuild::Unknown) {
      // Gamepad bindings + rumble hooks — must run after the game's input system
      // is initialized (and after the CRT FP package is ready).  Both are
      // build-aware: every address they need is in the per-build table, and the
      // FLInputManager / joystick-config struct offsets they poke were verified
      // identical on all three builds (see controller_setup_bindings).
      //
      // GOG was held back while its offsets were unchecked.  They are checked
      // now: FLInputManager::Init is instruction-identical to Steam's and its
      // `MOV ECX, controller_base_global+0x428` lands on the ported global
      // exactly, as do the +0x29f0..+0x2cc8 neighbours, and joystick_sync /
      // joystick_discover / rumble_dispatch compare with zero differences.
      uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
      controller_setup_bindings(base);
      if (g_rumbleEnabled) rumble_init(base);
   }
}

void lua_register_func(lua_State* L, const char* name, lua_CFunction fn)
{
   if (!g_lua.pushlstring || !g_lua.pushcclosure || !g_lua.settable) return;

   g_lua.pushlstring(L, name, strlen(name));
   g_lua.pushcclosure(L, fn, 0);
   g_lua.settable(L, -10001);
}

void lua_set_global_bool(lua_State* L, const char* name, bool value)
{
   if (!g_lua.pushlstring || !g_lua.pushboolean || !g_lua.settable) return;

   g_lua.pushlstring(L, name, strlen(name));
   g_lua.pushboolean(L, value ? 1 : 0);
   g_lua.settable(L, -10001);
}

void lua_set_global_string(lua_State* L, const char* name, const char* value)
{
   if (!g_lua.pushlstring || !g_lua.settable) return;

   g_lua.pushlstring(L, name, strlen(name));
   g_lua.pushlstring(L, value, strlen(value));
   g_lua.settable(L, -10001);
}

void lua_hooks_install(uintptr_t exe_base)
{
   // Requires the Lua VM API + init_state; no-ops on builds that lack them.
   if (!g_addr->init_state || !g_addr->g_lua_state_ptr || !g_addr->lua_pushcclosure)
      return;

   g_lua.pushcclosure = (fn_lua_pushcclosure)resolve(exe_base, g_addr->lua_pushcclosure);
   g_lua.pushlstring  = (fn_lua_pushlstring) resolve(exe_base, g_addr->lua_pushlstring);
   g_lua.settable     = (fn_lua_settable)    resolve(exe_base, g_addr->lua_settable);
   g_lua.tolstring    = (fn_lua_tolstring)   resolve(exe_base, g_addr->lua_tolstring);
   g_lua.pushnumber   = (fn_lua_pushnumber)  resolve(exe_base, g_addr->lua_pushnumber);
   g_lua.tonumber     = (fn_lua_tonumber)    resolve(exe_base, g_addr->lua_tonumber);
   g_lua.gettop       = (fn_lua_gettop)      resolve(exe_base, g_addr->lua_gettop);
   g_lua.pushnil      = (fn_lua_pushnil)     resolve(exe_base, g_addr->lua_pushnil);
   g_lua.pushboolean  = (fn_lua_pushboolean) resolve(exe_base, g_addr->lua_pushboolean);
   g_lua.toboolean    = (fn_lua_toboolean)   resolve(exe_base, g_addr->lua_toboolean);
   g_lua.touserdata        = (fn_lua_touserdata)       resolve(exe_base, g_addr->lua_touserdata);
   g_lua.pushlightuserdata = (fn_lua_pushlightuserdata) resolve(exe_base, g_addr->lua_pushlightuserdata);
   g_lua.isnumber          = (fn_lua_isnumber)          resolve(exe_base, g_addr->lua_isnumber);
   g_lua.gettable     = (fn_lua_gettable)    resolve(exe_base, g_addr->lua_gettable);
   g_lua.pcall        = (fn_lua_pcall)       resolve(exe_base, g_addr->lua_pcall);
   g_lua.rawgeti      = (fn_lua_rawgeti)     resolve(exe_base, g_addr->lua_rawgeti);
   g_lua.settop       = (fn_lua_settop)       resolve(exe_base, g_addr->lua_settop);
   g_lua.insert       = (fn_lua_insert)       resolve(exe_base, g_addr->lua_insert);
   if (g_addr->lua_newtable)
      g_lua.newtable  = (fn_lua_newtable)      resolve(exe_base, g_addr->lua_newtable);
   original_init_state = (fn_init_state)resolve(exe_base, g_addr->init_state);

   // Character-exit-vehicle event hook — needs the exit function AND the char
   // array globals for the pre-call scan.
   const bool wantCEV = g_addr->char_exit_vehicle && g_addr->char_array_base && g_addr->max_chars;
   if (wantCEV)
      original_char_exit_vehicle = (fn_char_exit_vehicle)resolve(exe_base, g_addr->char_exit_vehicle);

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   DetourAttach(&(PVOID&)original_init_state, hooked_init_state);
   if (wantCEV)
      DetourAttach(&(PVOID&)original_char_exit_vehicle, hooked_char_exit_vehicle);
   DetourTransactionCommit();

   // NOTE: lua_hooks_install runs from dllmain while the exe sections are still
   // PAGE_READWRITE (non-executable) — see dllmain.cpp.  Calling any game
   // function here (e.g. game_log / RedWarning::LogMessage) faults with an EXEC
   // access violation under DEP on Steam.  Any diagnostics added here must go to
   // a CRT log file rather than get_gamelog().

   // Patch the imm32 operand of the instruction that loads the hardcoded
   // "Load\\load" pointer (PUSH on modtools, MOV ECX on Steam) so it points at
   // g_loadDisplayPath instead.  Modders call SetLoadDisplayLevel() to
   // override the path before the load screen fires.
   // dllmain.cpp has already made all sections PAGE_READWRITE at this point.
   if (g_addr->enter_state_path_op) {
      g_enter_state_path_op_ptr  = (uint32_t*)resolve(exe_base, g_addr->enter_state_path_op);
      g_enter_state_path_op_orig = *g_enter_state_path_op_ptr;
      *g_enter_state_path_op_ptr = (uint32_t)(uintptr_t)g_loadDisplayPath;
   }

   if (g_build == GameBuild::Modtools) {
      // These installers still target raw modtools VAs / inline patch sites.
      grapple_fix_install(exe_base);
      DebugCommandRegistry::install(exe_base);
   }

   loading_screen_install(exe_base);          // guards internally (see lifecycle.cpp)
   flyer_boost_anim_install(exe_base);        // build-aware (all three), guards internally

   shield_channel_fix_install(exe_base);     // build-aware, guards internally
   droideka_shield_tracker_install(exe_base); // must follow the line above (outer hook)
   fp_anim_bank_install(exe_base);           // build-aware (all three), guards internally
   entity_carrier_fixes_install(exe_base);   // full set on modtools, bounds guards only elsewhere
   lua_create_entity_hook_install(exe_base); // guards internally
   ingame_movie_path_install(exe_base);      // build-aware (all three), guards internally

   // Barrel fire origin (WeaponCannon vtable patch) installs separately via
   // barrel_fire_origin_install() in weapon/barrel_fire_origin.cpp.
}

void lua_hooks_uninstall()
{
   loading_screen_uninstall();
   entity_carrier_fixes_uninstall();
   prone_system_uninstall();
   vehicle_view_toggle_uninstall();
   fp_anim_bank_uninstall();
   flyer_boost_anim_uninstall();
   cloth_collision_fix_uninstall();
   disguise_ext_uninstall();
   grapple_fix_uninstall();
   DebugCommandRegistry::uninstall();
   gc_visual_limits_uninstall();
   particle_batch_spill_uninstall();
   particle_density_uninstall();
   command_post_null_fix_uninstall();
   branch_region_debug_uninstall();
   branch_region_fix_uninstall();
   anim_bank_append_uninstall();
   shield_channel_fix_uninstall();
   aim_assist_uninstall();
   game_logging_uninstall();
   terrain_texture_fix_uninstall();
   barrel_fire_origin_uninstall();
   land_on_arrival_uninstall();
   flyer_sound_uninstall();
   droideka_ball_mode_uninstall();
   droideka_death_anim_uninstall();
   ingame_movie_path_uninstall();
   // Must run while the hooks are still live: it unlinks our omni lights from
   // the engine's global light lists first, and nothing else ever would.
   lightsaber_illumination_uninstall();
   // After it, so its deactivate_all() above is still covered by the guard.
   water_texture_count_fix_uninstall();
   red_light_stale_node_fix_uninstall();
   // detaching it first would unpick the chain from the middle.

   DetourTransactionBegin();
   DetourUpdateThread(GetCurrentThread());
   if (original_init_state)          DetourDetach(&(PVOID&)original_init_state, hooked_init_state);
   if (original_char_exit_vehicle)   DetourDetach(&(PVOID&)original_char_exit_vehicle, hooked_char_exit_vehicle);
   DetourTransactionCommit();

   // Restore the PUSH operand in LoadDisplay::EnterState
   if (g_enter_state_path_op_ptr && g_enter_state_path_op_orig) {
      DWORD oldProt;
      if (VirtualProtect(g_enter_state_path_op_ptr, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProt)) {
         *g_enter_state_path_op_ptr = g_enter_state_path_op_orig;
         VirtualProtect(g_enter_state_path_op_ptr, sizeof(uint32_t), oldProt, &oldProt);
      }
   }
}
