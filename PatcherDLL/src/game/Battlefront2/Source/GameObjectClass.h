#pragma once

#include <stdint.h>

#include "core/build_field.hpp"

// =============================================================================
// GameObjectClass - the ODF class a GameObject points at (GameObject +0x8).
// The members GameExt uses so far, not the whole struct. Offsets are from the
// class's start.
//
// mTargetFlags is a byte of ODF bitfields that sits 0x44 lower on Steam and GOG
// than on modtools. Bit 4 is IsNotTargetableByPlayer (hash 0x1B2D820B): its
// SetProperty case sets it, and PlayerController::Update skips a class with it
// set when a human is choosing a target:
//
//               SetProperty                          PlayerController::Update
//   modtools    MOV AL,[ESI+0x36C] / OR AL,0x10      TEST byte [EDX+0x36C],0x10
//               @00561246                            @0059C834
//   Steam       OR byte [ESI+0x328],0x10 @005368BF   TEST byte [EAX+0x328],0x10 @0061B677
//   GOG         OR byte [ESI+0x328],0x10 @0053762F   TEST byte [EAX+0x328],0x10 @0061C6E7
//
// Bit 5 of the same byte is mbHideHealthBar (Phantom PDB), which the HUD's own
// target events refuse; GameExt leaves that test to the HUD.
// =============================================================================

namespace layout::GameObjectClass {

inline constexpr Field<uint8_t> mTargetFlags{0x36C, 0x328};

constexpr uint8_t kNotTargetableByPlayer = 0x10;   // mTargetFlags bit 4

} // namespace layout::GameObjectClass
