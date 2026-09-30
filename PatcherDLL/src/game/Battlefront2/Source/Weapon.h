#pragma once

#include <stdint.h>

// =============================================================================
// Weapon - the per-instance weapon object (not WeaponClass, the ODF class).
//
// Offsets are from the start of the Weapon. Field names from the Phantom PDB;
// every offset read off Weapon::Weapon (the constructor), which writes them in
// declaration order on every build:
//
//   field                 modtools @      Steam @      GOG @ (same bytes as Steam)
//   mStart       0x60     0061d429        00677879     00678919  (run of stores
//   mClass       0x64     0061d42c        0067787c               from here to
//   mOwner       0x6C     0061d435        00677884               mReload 0x78)
//   mAimer       0x70     0061d43b        0067788a
//   mTrigger     0x74     0061d441        00677890
//   mAmmoCounter 0x88     0061d480        006778cc     0067896c
//   mState       0xB0     0061d4d1 (=0)   00677923     006789c3
//   mSoldierAnimationMap
//                0xC8     0061d508 (=-1)  00677959     006789f9
//
// Identical on all builds UP TO 0xD4 only. The three GameSounds that follow are
// 20 bytes each on modtools (0xD8/0xEC/0x100) and 8 on retail (0xD8/0xE0/0xE8),
// so every later field sits 0x24 lower on Steam and GOG (e.g. the mTarget
// handle: modtools 0x128, retail 0x104). Nothing here reads past 0xD4; a field
// from there on needs a per-build value.
//
// mStart, mClass and mRenderClass (0x68) are all set to the same WeaponClass*
// by the constructor. Most code reads mStart as "the weapon's class".
// =============================================================================

namespace layout::Weapon {

// TODO: Needs expanding, these are just all the parts shared by all builds.
constexpr uint32_t kStart               = 0x060; // WeaponClass* mStart
constexpr uint32_t kClass               = 0x064; // WeaponClass* mClass
constexpr uint32_t kOwner               = 0x06C; // Controllable* mOwner
constexpr uint32_t kAimer               = 0x070; // Aimer* mAimer
constexpr uint32_t kTrigger             = 0x074; // Trigger* mTrigger
constexpr uint32_t kAmmoCounter         = 0x088; // AmmoCounter* m_pAmmoCounter
constexpr uint32_t kState               = 0x0B0; // WeaponState mState
constexpr uint32_t kSoldierAnimationMap = 0x0C8; // MAP mSoldierAnimationMap

} // namespace layout::Weapon
