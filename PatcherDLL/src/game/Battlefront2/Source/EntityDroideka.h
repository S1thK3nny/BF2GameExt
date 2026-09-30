#pragma once

#include <stdint.h>

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
// Modtools is a DEBUG build and the retail builds are RELEASE, and this struct is
// laid out differently between them. Each offset is confirmed at two read sites:
//
//   field   modtools @                           Steam @
//   mClass  0x450  UpdateStateRolling 0x4e230c   0x438  UpdateStateRolling 0x4a2bf5
//                  GetMaxSpeed        0x4e2d90          GetMaxSpeed        0x4a6140
//   mState  0x1A74 UpdatePilot        0x4e826b   0x1A54 UpdatePilot        0x4a204c
//                  NextState          0x4ed611          NextState          0x4a3285
//
// GOG shares the Steam layout: UpdatePilot, SetProperty and Derive compare
// instruction for instruction between the two, displacements included.
// =============================================================================

namespace layout::Droideka {

constexpr int kStateIdle   = 0x00;
constexpr int kStateRollUp = 0x0B; // roll-up transition
constexpr int kStateBall   = 0x0C; // balled: collision swapped to mCollisionBall
constexpr int kStateUnroll = 0x0D; // unroll transition

constexpr uint32_t kClassModtools = 0x450;  // EntityDroidekaClass* mClass
constexpr uint32_t kClassRelease  = 0x438;
constexpr uint32_t kStateModtools = 0x1A74; // int mState
constexpr uint32_t kStateRelease  = 0x1A54;

} // namespace layout::Droideka
