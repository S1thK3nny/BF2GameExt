#pragma once

#include <stdint.h>

// =============================================================================
// Controllable - only the seat fields GameExt reads. Same on every build.
// Read off CommandFlyer::GetPassengerSlot, which tests the flyer's own
// Controllable (flyer +0x240) and each seat's (slot +0xC):
//
//   field       modtools 0x00648950        Steam/GOG 0x00477F10
//   mPilot      [EAX+0xd0] @006489a9       [EAX+0xd0] @00477f6a
//   mPlayerId   [ESI+0xe0] @006489b7       [ESI+0xe0] @00477f7b   (slot +0xC)
//   mPilotType  [EDI+0x384] @00648988      [EDX+0x384] @00477f4c  (flyer +0x240)
//
// A seat entity (PassengerSlot, MountedTurret) is EntityEx then Controllable,
// so its Controllable sits at +0xC: GetPassengerSlot's LEA [ESI+0xc] on both
// builds, and Steam MountedTurret::GetControllable 0x005A5210 (Trackable
// +0x24, LEA EAX,[ECX-0x18]).
// =============================================================================

namespace layout::Controllable {

constexpr uint32_t kPilot     = 0xD0;  // Controllable* mPilot
constexpr uint32_t kPlayerId  = 0xD4;  // int mPlayerId
constexpr uint32_t kPilotType = 0x144; // PilotType mPilotType

// PilotType
constexpr int kPilotSelf        = 1;
constexpr int kPilotVehicle     = 2;
constexpr int kPilotVehicleSelf = 4;

// Controllable inside a seat entity (PassengerSlot, MountedTurret).
constexpr uint32_t kSeatControllable = 0xC;

// Controllable::GetPilot(): null when nobody is in the seat.
inline void* GetPilot(void* c)
{
   const int type = *(int*)((char*)c + kPilotType);
   void* pilot = *(void**)((char*)c + kPilot);
   if (type == kPilotSelf || (type == kPilotVehicleSelf && pilot == c)) return nullptr;
   return pilot;
}

// Controllable::IsPilotable(): a seat a soldier can sit in.
inline bool IsPilotable(const void* c)
{
   const int type = *(const int*)((const char*)c + kPilotType);
   return type == kPilotVehicle || type == kPilotVehicleSelf;
}

} // namespace layout::Controllable
