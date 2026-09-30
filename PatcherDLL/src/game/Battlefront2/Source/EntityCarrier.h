#pragma once

#include <stdint.h>

#include "core/build_field.hpp"
#include "game/Battlefront2/Source/GameObject.h"
#include "game/Battlefront2/Source/GameSound.h"
#include "game/PebbleFL/Common/PblHandle.h"
#include "game/PebbleFL/Common/PblVector.h"

class EntityCarrierClass {
public:
   // mCargoInfo[4], set by the CargoNodeName / CargoNodeOffset properties.
   struct CargoInfo {
      uint32_t   mHash;
      PblVector3 mOffset;
   };
};
static_assert(sizeof(EntityCarrierClass::CargoInfo) == 0x10, "CargoInfo");

class EntityCarrier {
public:
   struct CargoSlot {
      PblVector3            mOffset;
      PblHandle<GameObject> mObject;
   };
};
static_assert(sizeof(EntityCarrier::CargoSlot) == 0x14, "CargoSlot");

// =============================================================================
// The members EntityCarrier and EntityCarrierClass add to their EntityFlyer
// bases, all of them (the bases are in EntityFlyer.h).
//
// The shipped builds differ from the PDB here: its EntityCarrierClass has
// mSwoopForwardBias and mFlyAwayForwardBias after mCargoCount, but both shipped
// constructors build the two cargo GameSounds right there instead.
//
//   EntityCarrier               modtools            Steam
//   mCargoArray                 0x1dd0 @004d754f    0x1d90 @00496f9e
//   mCargoCount                 0x1e20 @004d758c    0x1de0 @00497014
//   mRecentlyDetachedCargo      0x1e24              0x1de4             no shipped access;
//   mIgnoreDetachedCargoTimer   0x1e2c              0x1dec             the size leaves room
//   size                        0x1e30 @004d7f3e    0x1df0 @00402940   new / sMemoryPool
//
//   EntityCarrierClass          modtools            Steam              (ctor)
//   mCargoInfo                  0x1180 @004d70fb    0x10a0 @004975ae
//   mCargoCount                 0x11c0 @004d7111    0x10e0 @004975c9
//   mSoundCargoPickup           0x11c4 @004d70d7    0x10e4 @0049758f
//   mSoundCargoDropoff          0x11d8 @004d70ea    0x10ec @004975a1
//   size                        0x11f0 @0044d027    0x1100 @0049784a   new
//
// GOG runs the same code at the same addresses as Steam, with the same accesses
// and the same sizes.
// =============================================================================

namespace layout::EntityCarrier {

inline constexpr BuildSize kSize{0x1E30, 0x1DF0};

inline constexpr Field<::EntityCarrier::CargoSlot[4]> mCargoArray{0x1DD0, 0x1D90};
inline constexpr Field<int>                           mCargoCount{0x1E20, 0x1DE0};
inline constexpr Field<PblHandle<GameObject>>         mRecentlyDetachedCargo{0x1E24, 0x1DE4};
inline constexpr Field<float>                         mIgnoreDetachedCargoTimer{0x1E2C, 0x1DEC};

} // namespace layout::EntityCarrier

namespace layout::EntityCarrierClass {

inline constexpr BuildSize kSize{0x11F0, 0x1100};

inline constexpr Field<::EntityCarrierClass::CargoInfo[4]> mCargoInfo{0x1180, 0x10A0};
inline constexpr Field<int>                                mCargoCount{0x11C0, 0x10E0};
inline constexpr Field<::GameSound>                        mSoundCargoPickup{0x11C4, 0x10E4};
inline constexpr Field<::GameSound>                        mSoundCargoDropoff{0x11D8, 0x10EC};

} // namespace layout::EntityCarrierClass
