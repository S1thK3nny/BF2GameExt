#pragma once

#include <stdint.h>

// =============================================================================
// EntityWalker (and CommandWalker, which derives from it) - offsets from the
// start of the object: the `this` of EntityWalker::UpdateState and
// DoFootImpactEffects, and what Trackable::GetGameObject returns for it.
// EntityWalker_data starts at mClass. Modtools is a DEBUG build: it lays the
// data out 0x38 further in than retail, and 8 bytes longer before the flags
// (Phantom, a release layout, is 4 bytes shorter than retail there).
//
//   field              data     modtools  Steam/GOG  read at (modtools / retail)
//   mClass             +0x00    0x498     0x460      UpdateState 0x55B540 / 0x5029FD
//   mVelocity          +0x08    0x4A0     0x468      DoFootImpactEffects 0x555E2A / 0x500905
//   flags (dword)              0x2060    0x2020     UpdateState 0x55B550 / 0x502A0B:
//                                                    0x40 touched the ground this
//                                                    update, 0x80 jumping (Jump sets it,
//                                                    the next ground contact clears it)
//   mFootState                 0x20A4    0x2064     DoFootImpactEffects 0x556064 / 0x50091E:
//                                                    bit i, foot i landed this update
//   m_fGroundedTimer           0x20B4    0x2074     UpdateState 0x55B567 / 0x502A20:
//                                                    seconds since the ground, 0 on contact
//   mState                     0x20B8    0x2078     0x54F349 / 0x5034D8
//   mBoost (bit 0)             0x212C    0x20EC     UpdateState 0x55B674 / 0x502B46
//   mLastFootHeight[6]         0x2074    0x2034     DoFootImpactEffects 0x555B91 / 0x500754:
//                                                    each foot's height over the walker's
//                                                    origin, stored every update at
//                                                    0x555F39 / 0x500A58
//   mMinFootHeight[6]          0x208C    0x204C     0x555CDB / 0x500853: the lowest each
//                                                    foot has been, never reset
//
// EntityWalkerClass_data sits at class +0x6AC on modtools and +0x5E4 on retail:
//
//   mMaxSpeed          +0xBC    0x768     0x6A0      UpdateState 0x55B546 / 0x502A03
//   mBoostSpeed        +0xC0    0x76C     0x6A4      UpdateState 0x55B618 / 0x502AF1
//   mStompDetectionType +0x675  0xD21     0xC59      DoFootImpactEffects 0x555CBA / 0x500831
//   mNumFeet (byte)    +0x676   0xD22     0xC5A      DoFootImpactEffects 0x555B76 / 0x500736
//   mStompThreshold    +0x690   0xD3C     0xC74      DoFootImpactEffects 0x555CD5 / 0x50084B
//
// DoFootImpactEffects counts a foot's landing (sets its bit in mFootState, and
// plays the stomp effect and footstep sound) one of two ways, by the class's
// StompDetectionType:
//   0 (the default)  the update the foot, coming down, passes below its lowest
//                    height plus StompThreshold (0.15 unless the ODF sets it).
//   1                the first update it drops less than 0.1 in, once it is
//                    armed. Counting it disarms it (sets bit 24 + its number);
//                    a drop of more than 0.1 in a single update re-arms it.
//
// The feet are numbered in the order the ODF lists TerrainLeft and TerrainRight
// (EntityWalkerClass::SetProperty appends each to mFootPrimitives), left first
// in every stock walker, so even feet are left and odd feet right.
//
// mState runs EntityWalker::sStateTable (Phantom 0x00A8F0B0), compile-time
// constants: 0 standing, 1 and 2 turning on the spot (CheckTurn's inputs 4 and
// 5 from standing), 3 dying, 4 dead, 5 to 10 walking. Details:
// docs/RE/CameraShake.md.
// =============================================================================

namespace layout::Walker {

constexpr int      kStateTurnLeft  = 1;
constexpr int      kStateTurnRight = 2;
constexpr int      kStateDying     = 3;
constexpr int      kStateDead      = 4;
constexpr uint32_t kFlagJumping    = 0x80;
constexpr uint8_t  kBoosting       = 0x01;
constexpr int      kMaxFeet        = 6;     // EntityWalkerClass_data::mFootPrimitives[6]
constexpr uint32_t kMinFootHeight  = 0x18;  // mMinFootHeight[6], after mLastFootHeight[6]
constexpr int      kFootDownBit    = 24;    // type 1: foot i is disarmed while bit 24 + i is set
constexpr float    kStompDrop      = 0.1f;  // type 1: the drop in one update that re-arms a foot

struct Offsets {
   uint32_t cls;            // EntityWalkerClass* mClass
   uint32_t velocity;       // PblVector3 mVelocity
   uint32_t flags;          // jumping and ground contact
   uint32_t footState;      // feet that landed this update
   uint32_t airTime;        // m_fGroundedTimer
   uint32_t state;          // mState
   uint32_t boost;          // mBoost, bit 0
   uint32_t classMaxSpeed;  // in the class: MaxSpeed, then BoostSpeed
   uint32_t classNumFeet;   // in the class: mNumFeet, a byte
   uint32_t footHeight;     // mLastFootHeight[6], then mMinFootHeight[6]
   uint32_t classStompType; // in the class: mStompDetectionType, a byte
   uint32_t classStompThreshold; // in the class: mStompThreshold
};

constexpr Offsets kModtools = { 0x498, 0x4A0, 0x2060, 0x20A4, 0x20B4, 0x20B8, 0x212C, 0x768, 0xD22,
                                0x2074, 0xD21, 0xD3C };
constexpr Offsets kRelease  = { 0x460, 0x468, 0x2020, 0x2064, 0x2074, 0x2078, 0x20EC, 0x6A0, 0xC5A,
                                0x2034, 0xC59, 0xC74 }; // Steam and GOG

} // namespace layout::Walker
