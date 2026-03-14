#pragma once

#include <stdint.h>

// =============================================================================
// Cloth Collision Velocity Correction
//
// Hooks EntityCloth::EnforceCollisions to fix the Verlet velocity error that
// causes cloth to oscillate through collision primitives.  The collision
// functions modify the position buffer but never update old_pos, so the
// implicit Verlet velocity (pos - old_pos) fights the correction every frame.
//
// The fix snapshots positions before collision, then for any displaced particle
// kills the penetrating velocity component while preserving tangential sliding.
//
// Call cloth_collision_fix_install()   from lua_hooks_install().
// Call cloth_collision_fix_uninstall() from lua_hooks_uninstall().
// =============================================================================

void cloth_collision_fix_install(uintptr_t exe_base);
void cloth_collision_fix_uninstall();
