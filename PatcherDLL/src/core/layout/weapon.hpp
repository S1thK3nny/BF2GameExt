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
//
// mLastFireTime is past the split, so it has a value per build. Weapon::
// SignalFire stamps it with the mission time on every shot:
//   modtools 0x11C   MOV [ESI+0x11C],ECX     @0061C8EF
//   Steam    0xF8    MOVSS [ESI+0xF8],XMM1   @006796A5
//   GOG      0xF8    MOVSS [ESI+0xF8],XMM1   @0067A745
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

// Past the build split (see above).
constexpr uint32_t kLastFireTimeModtools = 0x11C; // float mLastFireTime
constexpr uint32_t kLastFireTimeRelease  = 0x0F8; // Steam, GOG

// Weapon's primary vtable, the same slots on every build. IsMelee: thiscall(),
// bool in AL, plain RET; only WeaponMelee answers true (modtools 0x00633B70,
// Steam 0x00687C40, GOG 0x00688CB0: MOV AL,1 / RET), so it is how a WeaponMelee
// is told apart, and how its vtable was found on Steam and GOG.
constexpr uint32_t kVt_Deflect    = 0x48;
constexpr uint32_t kVt_SignalFire = 0x4C;
constexpr uint32_t kVt_IsMelee    = 0x54;
constexpr uint32_t kVt_UpdateFire = 0xA4;

} // namespace layout::Weapon

// =============================================================================
// WeaponMelee - a lightsaber or other melee weapon (Phantom PDB names).
//
// m_pDamageData heads a list of DamageData, one per attack of the swing under
// way. WeaponMelee::UpdateFire lists each object an attack's blade reaches, once,
// before it asks the object to block (its vtable +0xD4, Deflect), so the list
// holds what was struck whether it blocked or not. A new DamageData goes on the
// front of the list (node->pNext = head; head = node):
//   modtools 0x1D8  MOV ECX,[EBX+0x1D8] / MOV [EAX+0x2C],ECX / MOV [EBX+0x1D8],EAX @00639350
//   Steam    0x1A8  MOV ECX,[EDI+0x1A8] / MOV [EDX+0x2C],ECX / MOV [EDI+0x1A8],EDX @0068C537
//   GOG      0x1A8  the same bytes                                                @0068D5C7
// DamageData is 0x30 bytes on every build: iNumObjects +0x08, apObject[8] +0x0C
// (written MOV [reg+reg*4+0xC] after a CMP with 8), pNext +0x2C.
// =============================================================================

namespace layout::WeaponMelee {

constexpr uint32_t kDamageDataModtools = 0x1D8; // DamageData* m_pDamageData
constexpr uint32_t kDamageDataRelease  = 0x1A8; // Steam, GOG

constexpr uint32_t kHitCount   = 0x08; // DamageData::iNumObjects
constexpr uint32_t kHitObjects = 0x0C; // DamageData::apObject[8], GameObject*
constexpr uint32_t kHitNext    = 0x2C; // DamageData::pNext
constexpr int      kHitMax     = 8;

} // namespace layout::WeaponMelee
