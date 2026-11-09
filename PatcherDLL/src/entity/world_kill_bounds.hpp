#pragma once

#include <stdint.h>

// =============================================================================
// Hardcoded out-of-world kill box.
//
// EntitySoldier::Update kills the soldier (host side only) when
//
//   velocity.y < -60  ||  y < -1000  ||  |x| > 2500  ||  |z| > 2500  ||  y > 1000
//
// and EntityItem::Update removes a falling pickup below y = -1000. These are
// literal constants, not SetWorldExtents (that only sizes the collision, path
// and hint grids).
//
// Other classes have floors of their own: flyers die below -5000, hovers,
// walkers, droidekas, droids, mines (detpacks), portable and defense grid
// turrets and powerups below -500, all ordnance below -2000,
// and OrdnanceBullet::Update drops any bullet-derived ordnance outside a static
// +/-5000 box. None of them has a top or side limit except that box.
//
// The compare operands are repointed at our own floats so a mission can move
// the box with SetWorldKillBounds. The other limits follow it but never get
// tighter than stock. Everything resets to stock on every mission load.
//
// All three builds.
// =============================================================================

void world_kill_bounds_install(uintptr_t exe_base);
void world_kill_bounds_uninstall();

// Back to stock values. Called from lua_hooks' init_state hook.
void world_kill_bounds_reset();

bool world_kill_bounds_active();

// horizontal = max |x| and |z|, bottom/top = y range, fall = max downward speed.
void world_kill_bounds_set(float horizontal, float bottom, float top, float fall);
void world_kill_bounds_get(float* horizontal, float* bottom, float* top, float* fall);
