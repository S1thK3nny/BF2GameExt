#pragma once

#include <stdint.h>

#include "core/build_field.hpp"

class EntityWalkerClass;

// =============================================================================
// EntityWalker (and CommandWalker, which derives from it) and EntityWalkerClass:
// the members GameExt uses so far, not the whole structs. Names from the PDB.
// Offsets are from the object start: the `this` of EntityWalker::UpdateState
// and DoFootImpactEffects, and what Trackable::GetGameObject returns for it.
// EntityWalker_data starts at mClass. Modtools is a debug build: it lays the
// data out 0x38 further in than release, and 8 bytes longer before the flags
// (Phantom, a release layout, is 4 bytes shorter than Steam there).
//
//   EntityWalker        modtools  Steam     read at (modtools / Steam)
//   mClass              0x0498    0x0460    UpdateState 0x55B540 / 0x5029FD
//   mVelocity           0x04a0    0x0468    DoFootImpactEffects 0x555E2A / 0x500905
//   mFlags (dword)      0x2060    0x2020    UpdateState 0x55B550 / 0x502A0B:
//                                           0x40 touched the ground this update,
//                                           0x80 jumping (Jump sets it, the next
//                                           ground contact clears it)
//   mLastFootHeight[6]  0x2074    0x2034    DoFootImpactEffects 0x555B91 / 0x500754:
//                                           each foot's height over the walker's
//                                           origin, stored every update at
//                                           0x555F39 / 0x500A58
//   mMinFootHeight[6]   0x208c    0x204c    0x555CDB / 0x500853: the lowest each
//                                           foot has been, never reset
//   mFootState          0x20a4    0x2064    DoFootImpactEffects 0x556064 / 0x50091E:
//                                           bit i, foot i landed this update
//   m_fGroundedTimer    0x20b4    0x2074    UpdateState 0x55B567 / 0x502A20:
//                                           seconds since the ground, 0 on contact
//   mState              0x20b8    0x2078    0x54F349 / 0x5034D8
//   mBoost (bit 0)      0x212c    0x20ec    UpdateState 0x55B674 / 0x502B46
//
//   EntityWalkerClass   modtools  Steam     (EntityWalkerClass_data at +0x6AC / +0x5E4)
//   mMaxSpeed           0x0768    0x06a0    UpdateState 0x55B546 / 0x502A03
//   mBoostSpeed         0x076c    0x06a4    UpdateState 0x55B618 / 0x502AF1
//   mStompDetectionType 0x0d21    0x0c59    DoFootImpactEffects 0x555CBA / 0x500831
//   mNumFeet (byte)     0x0d22    0x0c5a    DoFootImpactEffects 0x555B76 / 0x500736
//   mStompThreshold     0x0d3c    0x0c74    DoFootImpactEffects 0x555CD5 / 0x50084B
//
// GOG runs the same code at the same addresses as Steam; every access above is
// the same instruction there.
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

namespace layout::EntityWalker {

constexpr int      kStateTurnLeft  = 1;
constexpr int      kStateTurnRight = 2;
constexpr int      kStateDying     = 3;
constexpr int      kStateDead      = 4;
constexpr uint32_t kFlagJumping    = 0x80;  // mFlags
constexpr uint8_t  kBoosting       = 0x01;  // mBoost
constexpr int      kMaxFeet        = 6;     // EntityWalkerClass_data::mFootPrimitives[6]
constexpr int      kFootDownBit    = 24;    // type 1: foot i is disarmed while bit 24 + i is set
constexpr float    kStompDrop      = 0.1f;  // type 1: the drop in one update that re-arms a foot

inline constexpr Field<EntityWalkerClass*> mClass{0x498, 0x460};
inline constexpr Field<float[3]>           mVelocity{0x4A0, 0x468};
inline constexpr Field<uint32_t>           mFlags{0x2060, 0x2020};
inline constexpr Field<float[6]>           mLastFootHeight{0x2074, 0x2034};
inline constexpr Field<float[6]>           mMinFootHeight{0x208C, 0x204C};
inline constexpr Field<uint32_t>           mFootState{0x20A4, 0x2064};
inline constexpr Field<float>              m_fGroundedTimer{0x20B4, 0x2074};
inline constexpr Field<int>                mState{0x20B8, 0x2078};
inline constexpr Field<uint8_t>            mBoost{0x212C, 0x20EC};

} // namespace layout::EntityWalker

namespace layout::EntityWalkerClass {

inline constexpr Field<float>              mMaxSpeed{0x768, 0x6A0};
inline constexpr Field<float>              mBoostSpeed{0x76C, 0x6A4};
inline constexpr Field<uint8_t>            mStompDetectionType{0xD21, 0xC59};
inline constexpr Field<uint8_t>            mNumFeet{0xD22, 0xC5A};
inline constexpr Field<float>              mStompThreshold{0xD3C, 0xC74};

} // namespace layout::EntityWalkerClass
