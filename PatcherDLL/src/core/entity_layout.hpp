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

   // ---- Character-weapon Lua funcs (Get/SetCharacterWeapon, ammo) ------------
   // All from entity = the Controllable sub-object at struct+0x240 (the pointer
   // stored in the character slot at +0x148).  Derived from both EntitySoldier
   // ctors' weapon-build loops (modtools 0x5339d0 @0x533e20, Steam 0x4de850
   // @0x4dedc0) and cross-checked against the Phantom PDB field names.
   int classPtr;         // EntitySoldier::mClass        (EntitySoldierClass*)
   int aimer;            // EntitySoldier::mAimer        (embedded Aimer)
   int dualWieldFlag;    // byte, bit 0 = dual-wield/offhand active
   int weaponIndexMap;   // EntitySoldier::mWeaponIndex[2] (channel -> slot, 0xFF empty)
   int slot0Cache;       // cached slot-0 Weapon* (rewritten only if == old)
   int animator;         // EntitySoldier::mSoldierAnimator

   // EntitySoldierClass per-slot loadout arrays (release class layout differs
   // heavily from debug; same 0x20/0x20/0x8 spacing in both).
   int clsWeaponCount;   // int8   weapon count
   int clsWeaponClass;   // WeaponClass*[] per-slot class
   int clsWeaponAmmo;    // int[]  per-slot WeaponAmmo config word
   int clsWeaponChannel; // int8[] per-slot channel
};

// Offsets verified against the live binaries (MemExt :8192 / Steam :8193).
constexpr SoldierLayout kSoldierModtools = {
   /* mState */ 0x514, /* weaponIndex */ 0x512, /* weaponArray */ 0x4F0,
   /* soundPos2 */ 0x2AC, /* foleyProne */ 0xD8, /* aiHeightBaseRm */ 6 /* ESI */,
   /* classPtr */ 0x218, /* aimer */ 0x2D0, /* dualWieldFlag */ 0x24A,
   /* weaponIndexMap */ 0x510, /* slot0Cache */ 0x4D8, /* animator */ 0x520,
   /* clsWeaponCount */ 0x984, /* clsWeaponClass */ 0x93C,
   /* clsWeaponAmmo */ 0x95C, /* clsWeaponChannel */ 0x97C,
};
// Steam foleyProne: release FoleyFXSoldier packs 13 8-byte GameSound slots
// after a 0x18-byte header (debug uses 20-byte GameSounds from +0x24).  Grid
// anchored on 3 sites: Crouch plays mSquat @ +0x68 (0x4ED58B), Stand plays
// mStand @ +0x70 (0x4ED23E) and mLand @ +0x50 (0x4ED2E5) -> mProne = +0x60.
//
// Weapon-func offsets from the Steam ctor 0x4de850: mClass struct+0x440,
// mAimer +0x500, dual flag +0x472 bit0, mWeaponIndex +0x740, mSoldierAnimator
// +0x750, class arrays +0x748/+0x768/+0x788/+0x790 (all minus the 0x240
// Controllable base below).  slot0Cache inferred from the uniform -0x10 shift
// (write is compare-guarded, so a miss is a no-op, not a corruption).
constexpr SoldierLayout kSoldierSteam = {
   /* mState */ 0x504, /* weaponIndex */ 0x502, /* weaponArray */ 0x4E0,
   /* soundPos2 */ 0x29C, /* foleyProne */ 0x60, /* aiHeightBaseRm */ 7 /* EDI */,
   /* classPtr */ 0x200, /* aimer */ 0x2C0, /* dualWieldFlag */ 0x232,
   /* weaponIndexMap */ 0x500, /* slot0Cache */ 0x4C8, /* animator */ 0x510,
   /* clsWeaponCount */ 0x790, /* clsWeaponClass */ 0x748,
   /* clsWeaponAmmo */ 0x768, /* clsWeaponChannel */ 0x788,
};

// Active build's layout; defaults to modtools (set in game_build_select()).
extern const SoldierLayout* g_soldier;
