#pragma once

#include <stdint.h>

// =============================================================================
// ChaseCamera - the local player's game camera, and the Trackable it follows.
// Field names from the Phantom PDB; every offset read off ChaseCamera::SetupCamera
// on each build, where they are identical:
//
//   field                 offset  modtools 0x004A2440   Steam 0x00453D00   GOG 0x00453CE0
//   mOwner (Trackable*)   +0x0C   CMP/MOV [EBX+0xC]     CMP [ESI+0xC],0    CMP [ESI+0xC],0
//   mMatrix               +0x10   LEA EDI,[EBX+0x10]    LEA EAX,[ESI+0x10] LEA EAX,[ESI+0x10]
//   mPreShakeMatrix       +0x50   LEA EDI,[EBX+0x50]    (stored by field)  (stored by field)
//   mShake                +0x94   FLD [EBX+0x94]        MOVSS [EDI+0x94]   MOVSS [EDI+0x94]
//   mShakeSuppressUntil   +0x98   FCOMP [EBX+0x98]      COMISS [EDI+0x98]  COMISS [EDI+0x98]
//
// The shake queue, read off ChaseCamera::Update (vtable +0x04), which sums the
// amounts into mShake and runs each down by its decay:
//
//   field                 offset  modtools 0x004A2D70   Steam 0x00453820   GOG 0x00453800
//   mShakeCount           +0x9C   MOV EAX,[EBP+0x9C]    CMP [ESI+0x9C],EDI CMP [ESI+0x9C],EDI
//   mShakeAmount[4]       +0xA0   LEA EDX,[EBP+0xA0]    LEA EBX,[ESI+0xA0] LEA EBX,[ESI+0xA0]
//   mShakeDecay[4]        +0xB0   LEA EDI,[EBP+0xB0]    LEA ECX,[ESI+0xB0] LEA ECX,[ESI+0xB0]
//
// The Trackable's mTracker (+0x1C) is what SetupCamera hands to
// Tracker::IsFirstPersonView on every build. Details: docs/RE/CameraShake.md.
// =============================================================================

namespace layout::ChaseCamera {

constexpr uint32_t kOwner              = 0x0C; // Trackable* mOwner
constexpr uint32_t kMatrix             = 0x10; // PblMatrix mMatrix (64 bytes)
constexpr uint32_t kPreShakeMatrix     = 0x50; // PblMatrix mPreShakeMatrix
constexpr uint32_t kShake              = 0x94; // float mShake, the summed shake queue
constexpr uint32_t kShakeSuppressUntil = 0x98; // float mShakeSuppressUntil (mission time)
constexpr uint32_t kShakeCount         = 0x9C; // int mShakeCount, 0..4
constexpr uint32_t kShakeAmount        = 0xA0; // float mShakeAmount[4], each running down to 0
constexpr int      kShakeSlots         = 4;

constexpr uint32_t kTrackableTracker   = 0x1C; // Trackable::mTracker

} // namespace layout::ChaseCamera
