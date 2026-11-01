#pragma once

#include <stdint.h>

// =============================================================================
// Grenades with AlignVertical = 0 vanish once they stop.
//
// OrdnanceGrenade::Render draws on an identity rotation when the class's
// AlignVertical is set (the default). With it cleared it falls through to
// OrdnanceBullet::Render, which aims the model along the velocity:
//
//   PblMatrix::FromFwdUp(&m, &velocity, &PblVector3::UnitY, &pos)
//
// OrdnanceGrenade::Collide zeroes the velocity (and the gravity term) once a
// bounce leaves it under 1 m/s four times. D3DXVec3Normalize returns (0,0,0)
// for a zero vector on every CPU path (x87, SSE and P4 in modtools' static
// D3DX), so every axis of the matrix is zero and the model draws at zero scale
// for the rest of its fuse.
//
// A velocity that is exactly vertical (straight drops, the (0,-0.2,0) a water
// hit sets) is the other degenerate case. matrix_basis_fix already handles
// that one by swapping the up hint, so only the zero case is left here.
//
// The hook remembers the direction each grenade was last drawn along and, while
// the velocity is zero, draws it along that instead, so it lies where it landed
// in the pose it landed in. The velocity is swapped in only for the draw and
// restored straight after. A grenade that never moved draws facing +Z. The
// ctor hook forgets the entry when the ordnance pool hands that address out
// again.
//
// OrdnanceSticky (ClassLabel "sticky", e.g. the stock Ewok spear) has the same
// bug in its own Render: it derives from OrdnanceShell, not OrdnanceGrenade, so
// it gets the same pair of hooks. One that stuck to something draws on its
// stuck pose and is left alone.
//
// All three builds.
// =============================================================================

void grenade_rest_render_fix_install(uintptr_t exe_base);
void grenade_rest_render_fix_uninstall();
