#pragma once

#include <stdint.h>

#include "core/build_field.hpp"

// =============================================================================
// SoldierAnimator, SoldierAnimatorClass's action animation table,
// SoldierAnimation and the SoldierAnimationBank tables: the members GameExt uses
// so far, not the whole structs. Names from the PDB. None of these move between
// the debug and release layouts.
//
//   SoldierAnimator        offset  read in SetupPose (modtools / Steam / GOG)
//   mLegMatrix (rows)      0x00    right, up, forward, trans: the body's matrix;
//                                  the DIVE case dots mMovement with the right
//                                  and forward rows (Steam 0x63FE05..0x63FE3D)
//   mOwner                 0x50    0x57C56E / 0x6403A8 / 0x641448; the
//                                  EntitySoldier's start: SetupPose hands
//                                  NetGame::GetJoystickIndex the owner's +0x314,
//                                  Controllable::mPlayerId (+0xD4) of the
//                                  Controllable part at +0x240
//   mSoldierAction         0x70    0x57CA9E / 0x63FC28 / 0x640CC8; SoldierState
//   mWeaponAnimationMap    0x74    0x57C585 / 0x63FE83 / 0x640F23; MAP index
//   mMovement              0xAC    0x57CC43 / 0x63FDEB / 0x640E8B; the body's
//                                  move this frame, set by SetAction
//   mAction                0x1FEC  0x57CC19 / 0x63FDBA / 0x640E5A
//   mActionTime            0x1FF0  0x57CCB6 / 0x63FEC5 / 0x640F65
//
// SetAction(ROLL) sets mAction to DIVE and mActionTime to 0; SetupPose plays
// the action and adds the frame time to mActionTime once it has, so the first
// SetupPose of a roll sees 0. TUMBLE_RECOVER plays DIVE too.
//
// The action table: SoldierAnimatorClass::GetUpperBodyActionAnimation and
// GetLowerBodyActionAnimation, thiscall(action, map) with ECX the class
// (SoldierAnimatorClass::sInstance), RET 8, read
// [class + (map * 0x97 + action) * 8 + 0x24] (upper) and + 0x28 (lower) on
// every build (modtools 0x57DCA0 / 0x57DCC0, Steam 0x643940 / 0x643960, GOG
// 0x6449E0 / 0x644A00). Each entry is a SoldierAnimation*, null when the map
// has no animation for that action.
//
// The bank tables are static arrays SoldierAnimationBank fills as soldier
// classes load: s_aBank[16], s_aWeapon[20] and s_aMap[30], whose first entries
// are the stock human bank, its five weapons (rifle, bazooka, tool, pistol,
// melee; pistol and melee have the tool as parent, the tool and bazooka the
// rifle) and their five maps. A root bank or weapon is its own parent.
// AnimationFinder names an animation "<bank>_<weapon>_<anim>" and hashes it
// with PblTEMPHash. Addresses in game_addrs.hpp.
// =============================================================================

namespace layout::SoldierAnimator {

constexpr int32_t kStateRoll  = 5;    // SoldierState ROLL
constexpr int32_t kActionDive = 24;   // ActionAnimation DIVE ("diveforward")

inline constexpr Field<float[4]>  mLegMatrix_right{0x00};
inline constexpr Field<float[4]>  mLegMatrix_forward{0x20};
inline constexpr Field<uint8_t*>  mOwner{0x50};
inline constexpr Field<int32_t>   mSoldierAction{0x70};
inline constexpr Field<int32_t>   mWeaponAnimationMap{0x74};
inline constexpr Field<float[3]>  mMovement{0xAC};
inline constexpr Field<int32_t>   mAction{0x1FEC};
inline constexpr Field<float>     mActionTime{0x1FF0};

} // namespace layout::SoldierAnimator

namespace layout::SoldierAnimatorClass {

constexpr int32_t  kMaxMaps        = 30;     // MAP::MAX_MAPS
constexpr uint32_t kMapStride      = 0x97 * 8;
constexpr uint32_t kActionTable    = 0x24;   // upper; the lower is the next dword

// Where the action table keeps (map, action): the upper body's SoldierAnimation*,
// then the lower body's.
inline uint32_t action_entry(int32_t map, int32_t action)
{
   return (uint32_t)map * kMapStride + (uint32_t)action * 8 + kActionTable;
}

} // namespace layout::SoldierAnimatorClass

// SoldierAnimation: 0x10 bytes on every build. m_uiData packs the root
// velocity in its low 30 bits and the SCOPE (1 upper, 2 lower, 3 full) in the
// top two. mName is only set while animation debugging is on.
struct SoldierAnimation {
   void*       m_pZephyrAnim;
   uint32_t    m_uiData;
   void*       m_pAnimData;
   const char* mName;
};
static_assert(sizeof(SoldierAnimation) == 0x10);

namespace layout::SoldierAnimationBank {

constexpr int32_t  kMaxBanks     = 16;
constexpr int32_t  kMaxWeapons   = 20;
constexpr uint32_t kBankStride   = 0x2C;   // szName[32], uiNameHash, eParent, eLowResBank
constexpr uint32_t kBankParent   = 0x24;
constexpr uint32_t kWeaponStride = 0x30;   // szName[32], uiNameHash, eCombo, eParent, bHasAlertState
constexpr uint32_t kWeaponParent = 0x28;
constexpr uint32_t kNameLength   = 32;
constexpr uint32_t kMapStride    = 8;      // eBank, eWeapon

} // namespace layout::SoldierAnimationBank
