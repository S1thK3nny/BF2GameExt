#pragma once

#include <stdint.h>

#include "game/Battlefront2/Source/GameObject.h"
#include "game/PebbleFL/Common/PblHandle.h"
#include "game/PebbleFL/Common/PblVector.h"

// Only the nested types are modelled. The instance layouts differ between the
// debug and release builds; see CarrierLayout in entity/flyer_carrier_fixes.cpp.

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
