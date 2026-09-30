#pragma once

#include <stdint.h>

#include "core/build_field.hpp"

class Aimer;
class EntityFlyerClass;
class MountedTurret;
class PassengerSlot;
class ZephyrAnim;

// =============================================================================
// EntityFlyer and EntityFlyerClass: the members GameExt uses so far, not the
// whole structs. Names from the PDB. Offsets are from the object start (the
// pointer TakeOff and AttachCargo receive; Update gets +0x240, Render +0x94).
// Release sits 0x40 lower for these instance fields and 0xC8 lower for the
// class fields. Each offset is read off an instruction in a function that
// touches several of them:
//
//   EntityFlyer             modtools            Steam
//   mState                  0x05a4 @004fc77f    0x0564 @004aa9c0
//   mFlightRatio            0x05a8 @004fc7a0    0x0568 @004aa9ec
//   mLandedHeight           0x0600 @004d814b    0x05c0 @004974d1
//   mClass                  0x066c @004d757e    0x062c @00497005
//   mPassengerSlots         0x0670 @004f1c30    0x0630 @004aa81b
//   mTurret                 0x0680 @004d6899    0x0640 @004aa7e7
//   mNumTurrets             0x06a0 @004d67b4    0x0660 @004aa7df
//   mAimer                  0x06a8 @004d6774    0x0668 @004aace6
//   mZephyrPoseDyn          0x0ef0 @004f2a67    0x0eb0 @004aad81
//     .m_kAnim.m_pkAnim     0x1870 @004f6b0f    0x1830 @004ab225   Render, this+0x94
//   mTotalUpdateDt          0x1d00 @004f6a78    0x1cc0 @004ab158   Render, this+0x94
//   mPostCollision          0x1d10 @004f1ba8    0x1cd0 @004aad54
//
//   EntityFlyerClass        modtools            Steam
//   mAnimTakeoff            0x087c @004f5609    0x07b4 @004b676a
//   mTakeoffSpeed           0x08e8 @004f574d    0x0820 @004b6878
//   mLandingTime            0x08ec @004f5759    0x0824 @004b6882
//   mLandedHeight           0x08f4 @004d813b    0x082c @004974cb
//   mWeaponCount            0x0d48 @004f1bc1    0x0c80 @004aa7ad
//   mNumPassengerSlots      0x0e14 @004f1c24    0x0d4c @004aa812
//
// GOG runs the same code at the same addresses as Steam; every access above is
// the same instruction there.
// =============================================================================

namespace layout::EntityFlyer {

// EntityFlyer::State
constexpr int kLanded   = 0;
constexpr int kTakeoff  = 1;
constexpr int kFlying   = 2;
constexpr int kLanding  = 3;
constexpr int kCrashing = 4;
constexpr int kCrashed  = 5;

inline constexpr Field<int>                mState{0x5A4, 0x564};
inline constexpr Field<float>              mFlightRatio{0x5A8, 0x568};     // takeoff/landing clip progress
inline constexpr Field<float>              mLandedHeight{0x600, 0x5C0};    // includes carried cargo
inline constexpr Field<EntityFlyerClass*>  mClass{0x66C, 0x62C};
inline constexpr Field<PassengerSlot*[4]>  mPassengerSlots{0x670, 0x630};
inline constexpr Field<MountedTurret*[8]>  mTurret{0x680, 0x640};
inline constexpr Field<int8_t>             mNumTurrets{0x6A0, 0x660};
inline constexpr Field<Aimer*[4]>          mAimer{0x6A8, 0x668};
inline constexpr Field<uint8_t>            mZephyrPoseDyn{0xEF0, 0xEB0};   // ZephyrPoseDyn<32>
inline constexpr Field<const ZephyrAnim*>  mZephyrPoseDyn_pkAnim{0x1870, 0x1830}; // .m_kAnim.m_pkAnim
inline constexpr Field<float>              mTotalUpdateDt{0x1D00, 0x1CC0};
inline constexpr Field<uint8_t>            mPostCollision{0x1D10, 0x1CD0}; // EntityFlyer::PostCollision

} // namespace layout::EntityFlyer

namespace layout::EntityFlyerClass {

inline constexpr Field<const ZephyrAnim*>  mAnimTakeoff{0x87C, 0x7B4};
inline constexpr Field<float>              mTakeoffSpeed{0x8E8, 0x820};
inline constexpr Field<float>              mLandingTime{0x8EC, 0x824};
inline constexpr Field<float>              mLandedHeight{0x8F4, 0x82C};    // -(model bbox min Y)
inline constexpr Field<int>                mWeaponCount{0xD48, 0xC80};
inline constexpr Field<uint8_t>            mNumPassengerSlots{0xE14, 0xD4C};

} // namespace layout::EntityFlyerClass
