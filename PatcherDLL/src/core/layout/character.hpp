#pragma once

#include <stdint.h>

// =============================================================================
// Character - one entry of Character::sCharacters, the per-player/per-bot slot
// (not the soldier itself; mUnit points at the soldier's Controllable).
//
// Identical on all builds. Offsets are from the start of the Character.
// Field names from the Phantom PDB; every offset read off the instructions:
//
//   field      modtools (MemExt)          Steam                      GOG
//   size       GetCharacterUnit           GetCharacterUnit           same bytes
//   0x1B0      IMUL EAX,0x1b0 @0046ecb0   IMUL EAX,0x1b0 @0058fd02   @00590ca2
//   mUnit      GetCharacterUnit           GetCharacterUnit           same bytes
//   0x148      [EAX+0x148] @0046ecf9      [EAX+0x148] @0058fd0c      @00590cac
//   mVehicle   GetCharacterVehicle        Character::SetVehicle      same bytes
//   0x14C      [EAX+0x14c] @0046ede9      writes [ESI+0x14c] @004522be  @0045229e
//   mRemote    GetCharacterRemote         Character ctor zeroes 0x148/ same bytes
//   0x150      [EAX+0x150] @0046eed9      0x14c/0x150 in order @00450752 @00450732
//   mHeroFlag  ChangeTeam hero test       ChangeTeam hero test       same bytes
//   0x165      MOV AL,[EDI+0x165]         CMP byte [ESI+0x165],0     @00452318
//                                         @00452338
//
// Steam's SetVehicle also divides by 0x1B0 (0x4BDA12F7 >> 7) @004522a4. GOG was
// checked by searching its exe for the same instruction bytes as Steam.
// =============================================================================

namespace layout::Character {

constexpr uint32_t kSize     = 0x1B0; // stride of Character::sCharacters
constexpr uint32_t kUnit     = 0x148; // Controllable* mUnit: the soldier
constexpr uint32_t kVehicle  = 0x14C; // Controllable* mVehicle: what it boarded
constexpr uint32_t kRemote   = 0x150; // Controllable* mRemote: deployed remote unit
constexpr uint32_t kHeroFlag = 0x165; // bool mHeroFlag

} // namespace layout::Character
