#pragma once

#include <stdint.h>

// =============================================================================
// EntityFlyer - offsets from the start of the object: the `this` of
// EntityFlyer::DoTrick, and what Trackable::GetGameObject returns for it
// (GameObject is its primary base). EntityFlyer_data starts at +0x578 on
// modtools and +0x538 on Steam and GOG; the fields below keep their PDB order.
//
//   field                  data   modtools  Steam/GOG  read at
//   mVelocity (PblVector3) +0x08  0x580     0x540      RecalculateSpeed  mt 0x4F304A  st 0x4ABD07
//   mState                 +0x2C  0x5A4     0x564      TakeOff / Land (flyer_boost_animation.cpp)
//   flag byte              +0x7C  0x5F4     0x5B4      DoTrick TEST [..],3 mt 0x4F3DBF;
//                                                      bit 0x04 set while boost energy is spent
//   mTrick                 +0x98  0x610     0x5D0      DoTrick: 0 on a trick, -1.0 when refused
//   mClass                 +0xF4  0x66C     0x62C      DoTrick mt 0x4F3D19  st/gog 0x4B18FB
//
// Details: docs/RE/CameraShake.md.
// =============================================================================

namespace layout::Flyer {

// EntityFlyer::State (PDB enum): 0 LANDED, 1 TAKEOFF, 2 FLYING, 3 LANDING,
// 4 CRASHING, 5 CRASHED.
constexpr int     kStateLanded  = 0;
constexpr int     kStateFlying  = 2;
constexpr int     kStateLanding = 3;
constexpr uint8_t kFlagBoost   = 0x04; // flag byte: boosting

struct Offsets {
   uint32_t velocity;
   uint32_t state;
   uint32_t flags;
   uint32_t trick;
   uint32_t cls;
};

constexpr Offsets kModtools = { 0x580, 0x5A4, 0x5F4, 0x610, 0x66C };
constexpr Offsets kRelease  = { 0x540, 0x564, 0x5B4, 0x5D0, 0x62C }; // Steam and GOG

} // namespace layout::Flyer
