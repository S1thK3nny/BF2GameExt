#pragma once

#include <stdint.h>

// =============================================================================
// RedCamera - the render camera ChaseCamera::SetupCamera hands its matrix to.
// Field names from the Phantom PDB (RedCamera_data starts at +0x28); the same on
// every build, read where the projection divides the unzoomed field of view by
// the zoom:
//
//   field                      offset  modtools 0x007FEE50        Steam 0x006CBCD0          GOG 0x006CCD70
//   _Matrix                    +0x30   (RedCamera::SetMatrix copies 16 dwords to +0x30)
//   _fUnzoomedTanHalfFOVWidth  +0x138  FMUL [ECX+0x138]           MOVSS XMM0,[EDX+0x138]    MOVSS XMM0,[EDX+0x138]
//   _fZoom                     +0x140  FDIV [ECX+0x140] @7FEE57   DIVSS XMM1,[EDX+0x140]    DIVSS XMM1,[EDX+0x140]
//                                                                 @6CBCEB                   @6CCD8B
//   _fTanHalfFOVWidth          +0x144  FST [ECX+0x144]            (aim_assist.cpp reads it on every build)
//
// _fZoom is 1 unzoomed and grows as a scope zooms in.
//
// _MatrixInverse (+0x70), world to camera, is what places the reticule:
// ReticuleDisplay::Update transforms the aim point (mAimStart + mEyeDir * 1024)
// by it, then projects the result with near, far and field of view alone
// (RedCamera::TransformCameraPointToProjectionSpace, +0x130..+0x148):
//
//   modtools  0x00683270 calls 0x00413B24 -> 0x00678520: LEA EAX,[ESI+0x70] @67852B
//   Steam     0x00630650: LEA EAX,[ESI+0x70] @630883, D3DXVec3TransformCoord, 0x006CBFE0
//   GOG       0x006316F0: LEA EAX,[ESI+0x70] @631923, D3DXVec3TransformCoord
//
// RedCamera::SetMatrix recomputes it from _Matrix.
// =============================================================================

namespace layout::RedCamera {

constexpr uint32_t kMatrixInverse = 0x70;  // PblMatrix _MatrixInverse (64 bytes)
constexpr uint32_t kZoom          = 0x140; // float _fZoom

} // namespace layout::RedCamera
