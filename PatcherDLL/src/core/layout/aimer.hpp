#pragma once

#include <stdint.h>

// =============================================================================
// Aimer - the per-weapon aim state a Weapon reaches through Weapon::mAimer.
//
// Identical on all builds. Offsets are from the start of the Aimer. Field names
// from the Phantom PDB; every offset read off Aimer::SetSoldierInfo, which stores
// them in this order and nothing else (ECX = this, RET 8):
//
//   field          modtools 005ee9d0   Steam 0043d290      GOG 0043d280
//   mFirePos  0x88 LEA EDX,[ECX+0x88]  MOVQ [ECX+0x88]     MOVQ [ECX+0x88]
//                  @005ee9da           @0043d29a           @0043d28a
//   mRootPos  0x70 LEA EDX,[ECX+0x70]  MOVQ [ECX+0x70]     MOVQ [ECX+0x70]
//                  @005ee9f0           @0043d2af           @0043d29f
//   mDirection     LEA EDX,[ECX+0x48]  MOVQ [ECX+0x48]     MOVQ [ECX+0x48]
//             0x48 @005eea07           @0043d2c1           @0043d2b1
//   bDirect   0x29 MOV byte [ECX+0x29],1
//                  @005eea19           @0043d2cc           @0043d2bc
//
// GOG was checked by searching its exe for the same instruction bytes as Steam.
// SetSoldierInfo is bDirect's only writer, so a set bDirect means mRootPos and
// mDirection were authored together this turn. It writes mFirePos and mRootPos
// from the same vector; BarrelFireOrigin later moves only mFirePos.
// =============================================================================

namespace layout::Aimer {

constexpr uint32_t kDirect    = 0x29; // bool bDirect
constexpr uint32_t kDirection = 0x48; // PblVector3 mDirection
constexpr uint32_t kRootPos   = 0x70; // PblVector3 mRootPos
constexpr uint32_t kFirePos   = 0x88; // PblVector3 mFirePos

} // namespace layout::Aimer
