#pragma once

#include "core/build_field.hpp"
#include "game/PebbleFL/Common/PblMath.h"

class EntityDroidekaClass;

// =============================================================================
// EntityDroideka - the rolling droid. Offsets are from the start of the object:
// the `this` of EntityDroideka::UpdatePilot (entity vtable +0x120), and what
// Trackable::GetGameObject returns for it. NOT from the Controllable pointer a
// Character slot holds, which sits +0x240 into the object.
//
// The state machine is table driven: NextState(input) does
// SetState(sStateTable[mState].next[input]). Its ids are compile-time constants
// in the game's own state table, identical in modtools and Steam disassembly.
//
// Each offset is confirmed at two read sites:
//
//   field        modtools @                           Steam @
//   mClass       0x450  UpdateStateRolling 0x4e230c   0x438  UpdateStateRolling 0x4a2bf5
//                       GetMaxSpeed        0x4e2d90          GetMaxSpeed        0x4a6140
//   mState       0x1A74 UpdatePilot        0x4e826b   0x1A54 UpdatePilot        0x4a204c
//                       NextState          0x4ed611          NextState          0x4a3285
//   mTurnOffset  0x1A7C UpdateState        0x4e39b6   0x1A5C UpdateState        0x4a24e5
//                                          0x4e3a7c                             0x4a25b7
//
// GOG shares the Steam layout: UpdatePilot, SetProperty, Derive and UpdateState
// compare instruction for instruction between the two, displacements included.
// =============================================================================

namespace layout::EntityDroideka {

constexpr int kStateIdle   = 0x00;
constexpr int kStateRollUp = 0x0B; // roll-up transition
constexpr int kStateBall   = 0x0C; // balled: collision swapped to mCollisionBall
constexpr int kStateUnroll = 0x0D; // unroll transition

inline constexpr Field<EntityDroidekaClass*> mClass{0x450, 0x438};
inline constexpr Field<int>                  mState{0x1A74, 0x1A54};
inline constexpr Field<PblAngle>             mTurnOffset{0x1A7C, 0x1A5C};

} // namespace layout::EntityDroideka
