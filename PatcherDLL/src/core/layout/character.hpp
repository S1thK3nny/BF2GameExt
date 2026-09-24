#pragma once

#include <stdint.h>

// =============================================================================
// Character - one entry of Character::sCharacters, the per-player/per-bot slot
// (not the soldier itself; mUnit points at the soldier's Controllable).
//
// Identical on all builds. Offsets are from the start of the Character.
// Field names from the Phantom PDB; every offset read off the instructions:
//
//   field       modtools (MemExt)                     Steam
//   size 0x1B0  GetCharacterUnit  IMUL EAX,0x1b0      GetCharacterUnit IMUL EAX,0x1b0
//               @0046ecb0                             @0058fd02; SetVehicle divides
//                                                     by 0x1B0 (0x4BDA12F7 >> 7) @004522a4
//   mUnit       GetCharacterUnit  [EAX+0x148]         GetCharacterUnit [EAX+0x148]
//               @0046ecf9                             @0058fd0c
//   mVehicle    GetCharacterVehicle [EAX+0x14c]       Character::SetVehicle writes
//               @0046ede9                             [ESI+0x14c] @004522be
//   mRemote     GetCharacterRemote [EAX+0x150]        Character ctor zeroes 0x148/
//               @0046eed9                             0x14c/0x150 in order @00450752
//   mHeroFlag   ChangeTeam hero test                  same test
//               MOV AL,[EDI+0x165]                    CMP byte [ESI+0x165],0
//
// GOG is the Steam source through the same toolchain; its struct layouts match
// Steam's.
// =============================================================================

namespace layout::Character {

constexpr uint32_t kSize     = 0x1B0; // stride of Character::sCharacters
constexpr uint32_t kUnit     = 0x148; // Controllable* mUnit: the soldier
constexpr uint32_t kVehicle  = 0x14C; // Controllable* mVehicle: what it boarded
constexpr uint32_t kRemote   = 0x150; // Controllable* mRemote: deployed remote unit
constexpr uint32_t kHeroFlag = 0x165; // bool mHeroFlag

} // namespace layout::Character
