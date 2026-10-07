#pragma once

#include <stdint.h>

#include "core/build_field.hpp"

// =============================================================================
// EntitySoldierClass: the members GameExt uses so far, not the whole struct.
// Names from the PDB. Offsets are from the class's start, the pointer a
// soldier keeps at its Controllable part (+0x240) + g_soldier->classPtr.
// EntitySoldierClass_data sits as one block, 0xA0 below Phantom's on modtools
// and 0x294 below on release, like the weapon arrays in entity_layout.hpp.
//
// Each offset is read where EntitySoldier::Update sets up
// EntitySoldier::MoveJetHover for a soldier in JET_HOVER: the target speeds
// at full stick are forward mMaxSpeed x mThrustFactor[jet], backward
// mMaxStrafeSpeed x mThrustFactor[jet] and sideways mMaxStrafeSpeed x
// mStrafeFactor[jet].
//
//   EntitySoldierClass     modtools            Steam and GOG
//   mMaxSpeed              0x0890 @00548ae1    0x069c @004eba85
//   mMaxStrafeSpeed        0x0894 @00548ae9    0x06a0 @004eba8f
//   mThrustFactor[6]       0x08d8 @00548af7    0x06e4 @004ebabe
//   mStrafeFactor[6]       0x08f8 @00548b05    0x0704 @004ebab6
//
// mThrustFactor, mStrafeFactor and mTurnFactor are arrays of 8, one per
// ControlSpeed posture (ODF "ControlSpeed = <posture> thrust strafe turn");
// "jet" is index 6, defaults 0.3, 0.3 and 1.0. The jet hover uses it.
// GOG runs the same code at the same addresses as Steam.
// =============================================================================

namespace layout::EntitySoldierClass {

inline constexpr Field<float> mMaxSpeed{0x890, 0x69C};
inline constexpr Field<float> mMaxStrafeSpeed{0x894, 0x6A0};
inline constexpr Field<float> mThrustFactorJet{0x8D8, 0x6E4};   // mThrustFactor[6]
inline constexpr Field<float> mStrafeFactorJet{0x8F8, 0x704};   // mStrafeFactor[6]

} // namespace layout::EntitySoldierClass
