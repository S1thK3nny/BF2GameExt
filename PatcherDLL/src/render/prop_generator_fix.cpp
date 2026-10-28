#include "pch.h"
#include "prop_generator_fix.hpp"
#include "core/game_build.hpp"
#include "core/resolve.hpp"
#include "util/install_log.hpp"

#include <cmath>
#include <cstring>

// Retargets the layer reset CALL in PropGenerator::Cleanup at a stub that also
// clears the layer's RedLodData* at +0x90, and the SetupClusters CALL in
// PropGenerator::Read at a guard that skips it without a terrain grid. See
// prop_generator_fix.hpp.

namespace {

// Stock layer reset body on every build: MOV dword [ECX+0x80],0 ; RET
constexpr uint8_t kLayerResetBody[] = {0xC7, 0x81, 0x80, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0xC3};

uintptr_t s_origLayerReset = 0;
uint8_t*  s_relSite        = nullptr; // rel32 operand of the patched CALL
int32_t   s_relOrig        = 0;

using SetupClusters_t = void(__cdecl*)();

SetupClusters_t s_origSetup     = nullptr;
uint8_t*        s_setupRelSite  = nullptr;
int32_t         s_setupRelOrig  = 0;
const uint8_t*  s_terrainEnable = nullptr; // RedTerrain::_bEnable
const float*    s_metersPerPoint = nullptr; // RedTerrain::_fMetersPerPoint
uint8_t*        s_propsLoaded   = nullptr; // PropGenerator::mLoaded
const char*     s_missionScript = nullptr; // GameLoop::mMissionScript, char[0x40]

// ECX = layer. Register and flag transparent, like the original.
__declspec(naked) void layer_reset_stub()
{
   __asm {
      mov dword ptr [ecx + 0x90], 0
      jmp dword ptr [s_origLayerReset]
   }
}

// Follows the incremental-link JMP thunk modtools calls through.
uint8_t* call_target(uint8_t* call)
{
   uint8_t* target = call + 5 + *(int32_t*)(call + 1);
   if (*target == 0xE9) target = target + 5 + *(int32_t*)(target + 1);
   return target;
}

// The engine's buffer can lack a terminator at exactly 0x40 chars.
void copy_mission_name(char (&out)[0x41])
{
   size_t len = 0;
   if (s_missionScript) {
      __try {
         while (len < 0x40 && s_missionScript[len] != '\0') {
            out[len] = s_missionScript[len];
            ++len;
         }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
         len = 0;
      }
   }
   out[len] = '\0';
}

// Called in place of SetupClusters. Caller keeps nothing in volatile registers.
void __cdecl setup_clusters_guard()
{
   const bool  terrain = *s_terrainEnable != 0;
   const float scale   = *s_metersPerPoint;

   if (terrain && std::isfinite(scale) && scale > 0.0f) {
      s_origSetup();
      return;
   }

   // Same state as a map without foliage: no clusters, Update skipped.
   *s_propsLoaded = 0;

   char mission[0x41];
   copy_mission_name(mission);
   const char* name = mission[0] ? mission : "this map";

   if (!terrain) {
      warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                   "[PropGeneratorFix] %s has foliage props but its terrain has no INFO "
                   "block, so the prop grid has no size. Foliage props are off for this "
                   "map; without this fix the game crashes when the map is left or "
                   "restarted. Fix the map: Clear Foliage in WorldEdit, then re-munge.\n",
                   name);
   }
   else {
      warn_gamelog(RED_SEVERITY_ERROR, SRC_FILE, __LINE__,
                   "[PropGeneratorFix] %s has foliage props but a terrain grid scale of %g. "
                   "Foliage props are off for this map; without this fix the game crashes "
                   "when the map is left or restarted. Fix the terrain's grid scale and "
                   "re-munge.\n",
                   name, (double)scale);
   }
}

void install_layer_reset(uintptr_t exe_base)
{
   if (g_addr->prop_generator_cleanup_layer_reset_call == 0) return;

   uint8_t* const call =
      (uint8_t*)resolve(exe_base, g_addr->prop_generator_cleanup_layer_reset_call);

   if (*call != 0xE8) {
      install_log("[PropGeneratorFix] site %08X reads %02X, expected E8 -- left stock",
                  (unsigned)g_addr->prop_generator_cleanup_layer_reset_call, *call);
      return;
   }

   uint8_t* const target = call_target(call);

   if (std::memcmp(target, kLayerResetBody, sizeof(kLayerResetBody)) != 0) {
      install_log("[PropGeneratorFix] site %08X does not call the layer reset -- left stock",
                  (unsigned)g_addr->prop_generator_cleanup_layer_reset_call);
      return;
   }

   s_origLayerReset = (uintptr_t)target;
   s_relSite        = call + 1;
   s_relOrig        = *(int32_t*)s_relSite;
   *(int32_t*)s_relSite = (int32_t)((intptr_t)&layer_reset_stub - (intptr_t)(call + 5));
}

void install_setup_guard(uintptr_t exe_base)
{
   if (g_addr->prop_generator_read_setup_call == 0 ||
       g_addr->prop_generator_setup_clusters == 0 ||
       g_addr->red_terrain_enable == 0 ||
       g_addr->red_terrain_meters_per_point == 0 ||
       g_addr->prop_generator_loaded == 0) return;

   uint8_t* const call = (uint8_t*)resolve(exe_base, g_addr->prop_generator_read_setup_call);
   uint8_t* const setup = (uint8_t*)resolve(exe_base, g_addr->prop_generator_setup_clusters);

   if (*call != 0xE8 || call_target(call) != setup) {
      install_log("[PropGeneratorFix] site %08X does not call SetupClusters -- grid guard left stock",
                  (unsigned)g_addr->prop_generator_read_setup_call);
      return;
   }

   s_origSetup      = (SetupClusters_t)setup;
   s_terrainEnable  = (const uint8_t*)resolve(exe_base, g_addr->red_terrain_enable);
   s_metersPerPoint = (const float*)resolve(exe_base, g_addr->red_terrain_meters_per_point);
   s_propsLoaded    = (uint8_t*)resolve(exe_base, g_addr->prop_generator_loaded);
   if (g_addr->game_loop_mission_script)
      s_missionScript = (const char*)resolve(exe_base, g_addr->game_loop_mission_script);

   s_setupRelSite = call + 1;
   s_setupRelOrig = *(int32_t*)s_setupRelSite;
   *(int32_t*)s_setupRelSite =
      (int32_t)((intptr_t)&setup_clusters_guard - (intptr_t)(call + 5));
}

} // namespace

void prop_generator_fix_install(uintptr_t exe_base)
{
   install_layer_reset(exe_base);
   install_setup_guard(exe_base);
}

void prop_generator_fix_uninstall()
{
   // Sections are re-protected by now, so this cannot be a plain store.
   if (s_relSite) protected_write(s_relSite, &s_relOrig, sizeof(s_relOrig));
   s_relSite = nullptr;
   if (s_setupRelSite) protected_write(s_setupRelSite, &s_setupRelOrig, sizeof(s_setupRelOrig));
   s_setupRelSite = nullptr;
}
