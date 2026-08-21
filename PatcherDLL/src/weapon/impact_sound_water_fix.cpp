#include "pch.h"
#include "impact_sound_water_fix.hpp"
#include "core/resolve.hpp"
#include "core/game_build.hpp"

#include <float.h>
#include <stdio.h>
#include <stdarg.h>

// See impact_sound_water_fix.hpp for the mechanism and why this redirects one
// call site rather than hooking the callee.

bool g_impactSoundWaterFix = true;

namespace {

// bool __cdecl RedWater::GetWaterHeight(PblVector3* pos, float* out)
// Convention read from Ghidra on modtools (FUN_00843DB0, __cdecl, 2 args, caller
// cleans with ADD ESP,8) and cross-checked on Phantom.  Not inferred.
using fn_get_water_height_t = int(__cdecl*)(const void* pos, float* out);

fn_get_water_height_t g_origGetWaterHeight = nullptr;

uint8_t* s_relSite = nullptr;  // the rel32 operand, i.e. CALL opcode + 1
int32_t  s_relOrig = 0;

void install_log(const char* fmt, ...)
{
   // CRT only: install runs while every section is mapped PAGE_READWRITE, so
   // calling back into the engine's logger would EXEC-fault.
   FILE* f = nullptr;
   if (fopen_s(&f, "BF2GameExt.log", "a") != 0 || !f) return;
   va_list ap;
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

// Seed the out-param, then defer to the engine.  On a map WITH water the original
// overwrites our value with the real surface height, so underwater suppression is
// untouched; on a map without, the caller compares against -FLT_MAX and plays.
int __cdecl water_height_shim(const void* pos, float* out)
{
   if (out) *out = -FLT_MAX;
   return g_origGetWaterHeight(pos, out);
}

} // namespace

void impact_sound_water_fix_install(uintptr_t exe_base)
{
   if (!g_impactSoundWaterFix) return;
   if (g_addr->ordnance_collide_water_call == 0 || g_addr->red_water_get_water_height == 0)
      return;

   uint8_t* const call = reinterpret_cast<uint8_t*>(
      resolve(exe_base, g_addr->ordnance_collide_water_call));

   // Verify it really is `CALL rel32` and that it currently targets the function
   // we think it does.  A mismatch means the address moved; fail safe and leave
   // the game alone rather than rewriting an unrelated operand.
   if (*call != 0xE8) {
      install_log("[ImpactSound] site %08X reads %02X, expected E8 -- left stock",
                  (unsigned)g_addr->ordnance_collide_water_call, *call);
      return;
   }

   const uintptr_t expected = (uintptr_t)resolve(exe_base, g_addr->red_water_get_water_height);
   const int32_t   relOrig  = *reinterpret_cast<int32_t*>(call + 1);
   const uintptr_t actual   = (uintptr_t)(call + 5) + (intptr_t)relOrig;

   if (actual != expected) {
      install_log("[ImpactSound] site %08X targets %p, expected %p -- left stock",
                  (unsigned)g_addr->ordnance_collide_water_call, (void*)actual, (void*)expected);
      return;
   }

   g_origGetWaterHeight = reinterpret_cast<fn_get_water_height_t>(expected);

   // rel32 is relative to the END of the 5-byte instruction.
   const int32_t relNew =
      (int32_t)((intptr_t)&water_height_shim - (intptr_t)(call + 5));

   s_relSite = call + 1;
   s_relOrig = relOrig;
   *reinterpret_cast<int32_t*>(s_relSite) = relNew;
}

void impact_sound_water_fix_uninstall()
{
   // Sections are re-protected by now, so this cannot be a plain store.
   if (s_relSite) protected_write(s_relSite, &s_relOrig, sizeof(s_relOrig));
   s_relSite = nullptr;
}
