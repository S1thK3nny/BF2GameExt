#pragma once

#include <stdint.h>

// =============================================================================
// Impact sounds below world Y = 0.
//
// Ordnance::Collide picks between the water impact sound and the generic one by
// comparing the impact height against the water surface:
//
//   RedWater::GetWaterHeight(&pos, &waterHeight);   // bool return IGNORED
//   if (impact.y > waterHeight) Play(mSoundCollision);
//
// RedWater::GetWaterHeight writes *out ONLY when it succeeds -- the map must have
// a water layer AND WaterExists() must be true for that cell:
//
//   bool GetWaterHeight(PblVector3* pos, float* out) {
//      if (m_waterMap != NULL) {
//         ...clamp cell from pos.x / pos.z...
//         if (WaterExists(cellX, cellZ)) {
//            *out = m_fWaterHeightOffset + m_waterHeight;   // global, flat
//            return true;
//         }
//      }
//      return false;                                        // *out UNTOUCHED
//   }
//
// So on a map with no water, the stack slot is never written and the compare
// reads whatever was there -- provably the incoming CollisionObject* argument
// slot, a pointer bit-cast to float, i.e. a tiny POSITIVE denormal.  The gate
// therefore degenerates to `if (impact.y > ~0.0f) play`, and every impact below
// world Y = 0 is silent.  Weapons still hit; only the sound is lost.
//
// WHAT MAKES THIS CERTAIN rather than plausible: this is the ONLY one of the 24
// callers of GetWaterHeight in the image that omits the `TEST AL,AL / JZ` on the
// return value.  Every other caller checks it, and EntitySoldier::UpdateFoleyFX
// goes further and pre-stores -FLT_MAX into its local before calling.  This site
// does neither.
//
// THE FIX.  Retarget that one CALL's rel32 at a shim which seeds *out with
// -FLT_MAX and then tail-calls the real GetWaterHeight.  On a map with water the
// original still writes the true height, so genuine underwater suppression is
// preserved; on a map without, the comparison becomes `impact.y > -FLT_MAX`,
// which is true at any depth.
//
// This is deliberately NOT a Detours hook: hooking GetWaterHeight itself would
// change all 24 callers, 23 of which are already correct.  Only the broken call
// site is redirected.
//
// Sites (address is of the CALL OPCODE; the rel32 lives at +1):
//   modtools 0x0060526A -> 0x00843DB0
//   steam    0x005F7B5E -> 0x006CEA90
//   gog      0x005F8BFE -> 0x006CFB30      (byte-identical to steam: E8 2D6F0D00)
// =============================================================================

extern bool g_impactSoundWaterFix;

void impact_sound_water_fix_install(uintptr_t exe_base);
void impact_sound_water_fix_uninstall();
