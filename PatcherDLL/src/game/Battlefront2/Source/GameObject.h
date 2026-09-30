#pragma once

#include "game/PebbleFL/Common/PblHandle.h"

// Only what handles need so far. The PblHandled base (mHandleId at +0x204) is
// read at that offset by VehicleSpawn::UpdateSpawn on every build:
// modtools 0x00665C27, Steam 0x0066F4F9, GOG 0x00670599.
class GameObject;

template <> struct PblHandledOffset<GameObject> {
   static constexpr unsigned value = 0x200;
};
