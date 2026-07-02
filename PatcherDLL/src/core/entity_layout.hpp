#pragma once

#include <stdint.h>

// =============================================================================
// Per-build struct/codegen layout for the soldier subsystem.
//
// The MemExt build is a DEBUG build; Steam/GOG are RELEASE.  EntitySoldier is
// laid out differently between them: the EntitySoldier-specific block is shifted
// -0x10 on the release build (confirmed: mState 0x514->0x504, mWeaponIndex
// 0x512->0x502, mWeapon[8] 0x4F0->0x4E0, 2nd sound-pos 0x2AC->0x29C).  The
// Controllable base, SoldierAnimator and Weapon offsets ARE build-invariant and
// stay as constants at their use sites — only the differing fields live here.
//
// Selected by game_build_select() into g_soldier, parallel to g_addr.  See
// project memory "Debug vs release offsets" for the full offset map.
// =============================================================================

struct SoldierLayout {
   int mState;        // EntitySoldier::mState        (from entity = Controllable base)
   int weaponIndex;   // EntitySoldier::mWeaponIndex byte (low nibble = active slot)
   int weaponArray;   // EntitySoldier::mWeapon[8] base
   int soundPos2;     // 2nd position arg to the stance GameSound::Play
   int foleyProne;    // FoleyFXSoldier::mProne GameSound sub-offset (0 = not derived -> skip sound)

   // x86 ModR/M rm bits of the `this` register at the AI height-dispatch case
   // body in EntitySoldier::UpdateIndirect, used to assemble the Prone code-cave
   // stub (MOV EDX,[reg]; MOV ECX,reg).  ESI=6 on modtools, EDI=7 on Steam.
   uint8_t aiHeightBaseRm;
};

// Offsets verified against the live binaries (MemExt :8192 / Steam :8193).
constexpr SoldierLayout kSoldierModtools = {
   /* mState */ 0x514, /* weaponIndex */ 0x512, /* weaponArray */ 0x4F0,
   /* soundPos2 */ 0x2AC, /* foleyProne */ 0xD8, /* aiHeightBaseRm */ 6 /* ESI */,
};
constexpr SoldierLayout kSoldierSteam = {
   /* mState */ 0x504, /* weaponIndex */ 0x502, /* weaponArray */ 0x4E0,
   /* soundPos2 */ 0x29C, /* foleyProne */ 0 /* mProne not re-derived */, /* aiHeightBaseRm */ 7 /* EDI */,
};

// Active build's layout; defaults to modtools (set in game_build_select()).
extern const SoldierLayout* g_soldier;
