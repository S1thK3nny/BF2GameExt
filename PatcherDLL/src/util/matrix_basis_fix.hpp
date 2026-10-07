#pragma once

#include <stdint.h>

// =============================================================================
// PblMatrix basis builder guard.
//
// The engine builds orientation matrices from a forward and an up vector:
//   right = Normalize(cross(up, fwd)), up' = cross(fwd, right)
// When fwd is (nearly) parallel to up the cross product is too short to give a
// usable right axis, and anything built from that matrix is broken.
//
// Seen with an aimer frozen at AimerPitchLimits "-90 -90" on Steam: the release
// sin/cos table gives cos(-90) = 1.19e-8, so the aim direction is (0,-1,1.19e-8)
// against a (0,1,0) mount up, and the fired ordnance never shows. The debug
// build's table gives 1.31e-7 and the same ODF works there. Logged on Steam from
// Aimer::Update (0x0043E0DD) and its pose helper (0x0043D3D5).
//
// The hook substitutes the world axis least aligned with fwd as the up hint
// when the two are parallel. Every other call passes straight through.
//
// All three builds.
// =============================================================================

void matrix_basis_fix_install(uintptr_t exe_base);
void matrix_basis_fix_uninstall();
